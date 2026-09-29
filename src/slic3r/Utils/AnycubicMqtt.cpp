#include "AnycubicMqtt.hpp"

#include <sstream>
#include <iomanip>
#include <chrono>
#include <array>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "Http.hpp"
#include "libslic3r/miniz_extension.hpp"

namespace pt = boost::property_tree;
namespace fs = boost::filesystem;
namespace asio = boost::asio;

namespace Slic3r {

namespace {

// --- small crypto/encoding helpers, all via OpenSSL (already a hard dependency) ---

std::string md5_hex(const std::string &data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_MD_CTX   *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i)
        oss << std::setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

// Streams the file through MD5 rather than loading it whole -- a .gcode.3mf can be tens of MB.
bool md5_hex_file(const fs::path &path, std::string &out_hex)
{
    fs::ifstream in(path, std::ios::binary);
    if (!in) return false;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_MD_CTX   *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);

    std::array<char, 1 << 16> buf{};
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0)
        EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(in.gcount()));

    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i)
        oss << std::setw(2) << static_cast<int>(digest[i]);
    out_hex = oss.str();
    return true;
}

std::string random_alphanumeric(int length)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::vector<unsigned char> raw(length);
    RAND_bytes(raw.data(), length);
    std::string out(length, '0');
    for (int i = 0; i < length; ++i)
        out[i] = alphabet[raw[i] % (sizeof(alphabet) - 1)];
    return out;
}

// A v4-shaped random identifier -- doesn't need to be a cryptographically real UUID, the
// printer only uses msgid to correlate a response, and every real msgid we captured was
// effectively just a random token.
std::string random_msgid()
{
    std::ostringstream oss;
    oss << random_alphanumeric(8) << "-" << random_alphanumeric(4) << "-4"
        << random_alphanumeric(3) << "-" << random_alphanumeric(4) << "-" << random_alphanumeric(12);
    return oss.str();
}

int64_t unix_millis_now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string base64_decode(const std::string &in)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO *mem = BIO_new_mem_buf(in.data(), static_cast<int>(in.size()));
    mem = BIO_push(b64, mem);

    std::string out(in.size(), '\0');
    int n = BIO_read(mem, out.data(), static_cast<int>(out.size()));
    BIO_free_all(mem);
    out.resize(n > 0 ? n : 0);
    return out;
}

// AES-128-CBC, PKCS7 padding -- matches LanCredentials.cs's DecryptAesCbc exactly (key and
// IV are both used as raw ASCII bytes of a 16-character string, not hex/base64-decoded).
bool aes128_cbc_decrypt(const std::string &cipher, const std::string &key16, const std::string &iv16, std::string &out)
{
    if (key16.size() != 16 || iv16.size() != 16) return false;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                            reinterpret_cast<const unsigned char *>(key16.data()),
                            reinterpret_cast<const unsigned char *>(iv16.data())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    std::vector<unsigned char> outbuf(cipher.size() + EVP_CIPHER_block_size(EVP_aes_128_cbc()));
    int len1 = 0, len2 = 0;
    if (EVP_DecryptUpdate(ctx, outbuf.data(), &len1,
                           reinterpret_cast<const unsigned char *>(cipher.data()),
                           static_cast<int>(cipher.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    if (EVP_DecryptFinal_ex(ctx, outbuf.data() + len1, &len2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    EVP_CIPHER_CTX_free(ctx);

    out.assign(reinterpret_cast<char *>(outbuf.data()), len1 + len2);
    return true;
}

// --- credential discovery: GET /info, signed POST /ctrl, decrypt -- port of LanCredentials.cs ---

struct LanCredentials
{
    std::string broker_host;
    int         broker_port{8883};
    std::string device_id;
    std::string username;
    std::string password;
    std::string device_crt; // PEM
    std::string device_pk;  // PEM
    std::string model_id;
};

bool parse_broker_uri(const std::string &uri, std::string &host, int &port)
{
    // Expected shape: mqtts://host:port
    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos) return false;
    auto rest = uri.substr(scheme_end + 3);
    auto colon = rest.find(':');
    if (colon == std::string::npos) { host = rest; return true; }
    host = rest.substr(0, colon);
    try { port = std::stoi(rest.substr(colon + 1)); } catch (...) { return false; }
    return true;
}

bool discover_credentials(const std::string &printer_host, LanCredentials &out, std::string &err)
{
    std::string info_body;
    bool ok = true;
    Http::get((boost::format("http://%1%:18910/info") % printer_host).str())
        .on_error([&](std::string body, std::string error, unsigned status) {
            ok = false;
            err = (boost::format("GET /info failed: %1% (HTTP %2%)") % error % status).str();
        })
        .on_complete([&](std::string body, unsigned) { info_body = std::move(body); })
        .perform_sync();
    if (!ok) return false;

    std::string token, ctrl_url;
    try {
        std::stringstream ss(info_body);
        pt::ptree root;
        pt::read_json(ss, root);
        token    = root.get<std::string>("token");
        ctrl_url = root.get<std::string>("ctrlInfoUrl");
    } catch (const std::exception &ex) {
        err = std::string("Could not parse /info response: ") + ex.what();
        return false;
    }
    if (token.size() < 32) {
        err = "Printer's /info token was shorter than expected";
        return false;
    }

    auto timestamp = std::to_string(unix_millis_now());
    auto nonce     = random_alphanumeric(6);
    auto device_id = "KobraSlicer-" + random_alphanumeric(8);
    auto sign      = md5_hex(md5_hex(token.substr(0, 16)) + timestamp + nonce);

    std::string ctrl_body;
    auto ctrl_full_url = (boost::format("%1%?ts=%2%&nonce=%3%&sign=%4%&did=%5%")
                           % ctrl_url % timestamp % nonce % sign % device_id).str();
    // A bodyless POST (this endpoint reads only its query-string params) hits a bug in
    // Http.cpp: http_perform() always wires up CURLOPT_READFUNCTION for a file upload, and
    // with no form/mime/postfields set curl invokes it with a null CURLOPT_READDATA -- an
    // access-violation crash. A throwaway single-space body avoids the empty-postfields
    // branch without touching the shared Http.cpp every other print host depends on.
    Http::post(ctrl_full_url)
        .set_post_body(std::string(" "))
        .on_error([&](std::string body, std::string error, unsigned status) {
            ok = false;
            err = (boost::format("POST /ctrl failed: %1% (HTTP %2%)") % error % status).str();
        })
        .on_complete([&](std::string body, unsigned) { ctrl_body = std::move(body); })
        .perform_sync();
    if (!ok) return false;

    std::string cipher_b64, iv, model_id;
    try {
        std::stringstream ss(ctrl_body);
        pt::ptree root;
        pt::read_json(ss, root);
        int code = root.get<int>("code");
        if (code != 200) {
            err = (boost::format("/ctrl returned code %1%") % code).str();
            return false;
        }
        auto &data = root.get_child("data");
        cipher_b64 = data.get<std::string>("info");
        iv         = data.get<std::string>("token");
    } catch (const std::exception &ex) {
        err = std::string("Could not parse /ctrl response: ") + ex.what();
        return false;
    }

    auto key = token.substr(16, 16);
    std::string plain_json;
    if (!aes128_cbc_decrypt(base64_decode(cipher_b64), key, iv, plain_json)) {
        err = "Could not decrypt /ctrl response (AES-128-CBC)";
        return false;
    }

    try {
        std::stringstream ss(plain_json);
        pt::ptree root;
        pt::read_json(ss, root);
        std::string broker = root.get<std::string>("broker");
        if (!parse_broker_uri(broker, out.broker_host, out.broker_port)) {
            err = "Could not parse broker URI: " + broker;
            return false;
        }
        out.device_id = root.get<std::string>("deviceId");
        out.username  = root.get<std::string>("username");
        out.password  = root.get<std::string>("password");
        out.device_crt = root.get<std::string>("devicecrt");
        out.device_pk  = root.get<std::string>("devicepk");
        out.model_id   = root.get<std::string>("modeId");
    } catch (const std::exception &ex) {
        err = std::string("Could not parse decrypted credentials: ") + ex.what();
        return false;
    }
    return true;
}

// --- minimal MQTT v3.1.1 wire encoding: CONNECT + PUBLISH (QoS 0), nothing else ---

void write_remaining_length(std::string &out, size_t len)
{
    do {
        uint8_t byte = static_cast<uint8_t>(len % 128);
        len /= 128;
        if (len > 0) byte |= 0x80;
        out.push_back(static_cast<char>(byte));
    } while (len > 0);
}

void write_utf8_string(std::string &out, const std::string &s)
{
    uint16_t len = static_cast<uint16_t>(s.size());
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
    out += s;
}

std::string build_connect_packet(const std::string &client_id, const std::string &username, const std::string &password)
{
    std::string variable_header;
    write_utf8_string(variable_header, "MQTT");
    variable_header.push_back(4);    // protocol level 4 = MQTT 3.1.1
    variable_header.push_back(static_cast<char>(0xC2)); // username + password + clean session
    variable_header.push_back(0);    // keep-alive MSB
    variable_header.push_back(60);   // keep-alive 60s LSB -- irrelevant, connection closes right after publish

    std::string payload;
    write_utf8_string(payload, client_id);
    write_utf8_string(payload, username);
    write_utf8_string(payload, password);

    std::string packet;
    packet.push_back(static_cast<char>(0x10)); // CONNECT
    write_remaining_length(packet, variable_header.size() + payload.size());
    packet += variable_header;
    packet += payload;
    return packet;
}

std::string build_publish_packet(const std::string &topic, const std::string &payload)
{
    std::string variable_header;
    write_utf8_string(variable_header, topic); // QoS 0: no packet identifier

    std::string packet;
    packet.push_back(static_cast<char>(0x30)); // PUBLISH, QoS 0, no DUP/RETAIN
    write_remaining_length(packet, variable_header.size() + payload.size());
    packet += variable_header;
    packet += payload;
    return packet;
}

// SUBSCRIBE (QoS 0 grant requested), single topic filter.
std::string build_subscribe_packet(uint16_t packet_id, const std::string &topic_filter)
{
    std::string variable_header;
    variable_header.push_back(static_cast<char>((packet_id >> 8) & 0xFF));
    variable_header.push_back(static_cast<char>(packet_id & 0xFF));

    std::string payload;
    write_utf8_string(payload, topic_filter);
    payload.push_back(0); // requested QoS 0

    std::string packet;
    packet.push_back(static_cast<char>(0x82)); // SUBSCRIBE, fixed header flags 0010 required by spec
    write_remaining_length(packet, variable_header.size() + payload.size());
    packet += variable_header;
    packet += payload;
    return packet;
}

// Unsubscribes once a query is done, so a held-open session (see AnycubicLink::attach_session())
// doesn't keep accumulating unread reports on a topic nothing is reading anymore.
std::string build_unsubscribe_packet(uint16_t packet_id, const std::string &topic_filter)
{
    std::string variable_header;
    variable_header.push_back(static_cast<char>((packet_id >> 8) & 0xFF));
    variable_header.push_back(static_cast<char>(packet_id & 0xFF));

    std::string payload;
    write_utf8_string(payload, topic_filter);

    std::string packet;
    packet.push_back(static_cast<char>(0xA2)); // UNSUBSCRIBE, fixed header flags 0010 required by spec
    write_remaining_length(packet, variable_header.size() + payload.size());
    packet += variable_header;
    packet += payload;
    return packet;
}

// Reads exactly one MQTT control packet (fixed header + remaining-length-encoded body),
// applying a receive timeout so a query that never gets a real response doesn't hang forever.
// Returns false on timeout or any read error -- out_type/out_body are only valid on true.
bool read_one_mqtt_packet(asio::ssl::stream<asio::ip::tcp::socket> &stream, int timeout_ms,
                           uint8_t &out_type, std::string &out_body)
{
    auto &sock = stream.next_layer();
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#endif
    boost::system::error_code ec;

    unsigned char first_byte = 0;
    size_t n = asio::read(stream, asio::buffer(&first_byte, 1), ec);
    if (ec || n != 1)
        return false;
    out_type = static_cast<uint8_t>(first_byte >> 4);

    // Decode the variable-length "remaining length" field, one byte at a time.
    size_t remaining_length = 0;
    size_t multiplier = 1;
    for (;;) {
        unsigned char len_byte = 0;
        n = asio::read(stream, asio::buffer(&len_byte, 1), ec);
        if (ec || n != 1)
            return false;
        remaining_length += (len_byte & 0x7F) * multiplier;
        if ((len_byte & 0x80) == 0)
            break;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128)
            return false; // malformed
    }

    out_body.resize(remaining_length);
    if (remaining_length > 0) {
        n = asio::read(stream, asio::buffer(&out_body[0], remaining_length), ec);
        if (ec || n != remaining_length)
            return false;
    }
    return true;
}

// Loads a client certificate + private key from in-memory PEM strings (the credentials
// come from a decrypted JSON blob, never touch disk) and disables server certificate
// verification -- matches Kobra LAN Monitor's own WithCertificateValidationHandler(_ =>
// true): this is a LAN-only control channel behind the user's own network, the same trust
// model Anycubic's own apps use.
bool configure_client_cert(asio::ssl::context &ctx, const std::string &crt_pem, const std::string &key_pem, std::string &err)
{
    ctx.set_verify_mode(asio::ssl::verify_none);

    BIO *crt_bio = BIO_new_mem_buf(crt_pem.data(), static_cast<int>(crt_pem.size()));
    X509 *cert = PEM_read_bio_X509(crt_bio, nullptr, nullptr, nullptr);
    BIO_free(crt_bio);
    if (!cert) { err = "Could not parse device certificate"; return false; }
    if (SSL_CTX_use_certificate(ctx.native_handle(), cert) != 1) {
        X509_free(cert);
        err = "Could not load device certificate into TLS context";
        return false;
    }
    X509_free(cert);

    BIO *key_bio = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
    BIO_free(key_bio);
    if (!pkey) { err = "Could not parse device private key"; return false; }
    if (SSL_CTX_use_PrivateKey(ctx.native_handle(), pkey) != 1) {
        EVP_PKEY_free(pkey);
        err = "Could not load device private key into TLS context";
        return false;
    }
    EVP_PKEY_free(pkey);
    return true;
}

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:   out += c;
        }
    }
    return out;
}

// paint_index/paint_color describe the file's own slicer-side filament; ams_index/ams_color
// describe the real, physical ACE Pro tray the user actually confirmed via the send dialog
// (AnycubicLink.cpp) -- two independent fields, not a single value to guess or auto-query.
// ams_index falls back to paint_index only when no tray was confirmed (gcode-only fallback).
// See ANYCUBIC_INTEGRATION_NOTES.md for the real capture this was confirmed against.
struct PaintInfo
{
    std::string material_type = "PLA";
    int         r = 255, g = 255, b = 255;
    int         paint_index   = 0;
    bool        found         = false;
    int         ams_index = -1;
    int         ams_r = 1, ams_g = 1, ams_b = 1;
    // filesize (print/start) and gcode_size (buried/PrintStart) are different numbers: filesize
    // is the archive's own compressed disk size, gcode_size is the decompressed internal
    // plate_N.gcode text size. 0 here means "couldn't read it", not a fabricated size.
    size_t      real_gcode_size = 0;
};

// Reads the paint_info this fork's own GCode.cpp writes into the uploaded file's own gcode
// header (see the "; ams_info = begin" block) directly out of the .gcode.3mf we just sent, so
// what this class tells the ACE Pro via MQTT can never disagree with what's actually in the
// file the printer is about to parse. Only the first entry is used -- multi-filament AMS
// mapping isn't handled yet, matching this project's own single-filament testing so far.
PaintInfo extract_paint_info(const std::string &source_file_path)
{
    PaintInfo info;

    mz_zip_archive archive;
    memset(&archive, 0, sizeof(archive));
    if (!open_zip_reader(&archive, source_file_path))
        return info;

    int file_index = mz_zip_reader_locate_file(&archive, "Metadata/plate_1.gcode", nullptr, 0);
    if (file_index < 0) {
        close_zip_reader(&archive);
        return info;
    }

    size_t extracted_size = 0;
    void  *data = mz_zip_reader_extract_to_heap(&archive, file_index, &extracted_size, 0);
    close_zip_reader(&archive);
    if (data == nullptr)
        return info;

    info.real_gcode_size = extracted_size;
    std::string gcode(static_cast<const char *>(data), extracted_size);
    mz_free(data);

    const std::string marker = "; paint_info = [";
    size_t pos = gcode.find(marker);
    if (pos == std::string::npos)
        return info;
    size_t line_end = gcode.find('\n', pos);
    std::string line = gcode.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);

    // Minimal, purpose-built parse of our own known-shape output -- not a general JSON parser.
    // {"material_type":"PLA","paint_color":[255,236,61],"paint_index":0}
    auto find_string_field = [&](const std::string &key) -> std::string {
        size_t p = line.find("\"" + key + "\":\"");
        if (p == std::string::npos) return {};
        p += key.size() + 4;
        size_t e = line.find('"', p);
        return e == std::string::npos ? std::string() : line.substr(p, e - p);
    };
    auto find_int_field = [&](const std::string &key) -> long {
        size_t p = line.find("\"" + key + "\":");
        if (p == std::string::npos) return -1;
        p += key.size() + 3;
        return std::strtol(line.c_str() + p, nullptr, 10);
    };

    std::string material = find_string_field("material_type");
    long        idx       = find_int_field("paint_index");
    size_t      color_pos = line.find("\"paint_color\":[");
    if (material.empty() || idx < 0 || color_pos == std::string::npos)
        return info;

    color_pos += 15; // strlen("\"paint_color\":[")
    int rgb[3] = {255, 255, 255};
    const char *p = line.c_str() + color_pos;
    for (int i = 0; i < 3; ++i) {
        char *endp = nullptr;
        rgb[i] = static_cast<int>(std::strtol(p, &endp, 10));
        if (endp == p) return info;
        p = endp;
        while (*p == ',' || *p == ' ') ++p;
    }

    info.material_type = material;
    info.paint_index    = static_cast<int>(idx);
    info.r = rgb[0]; info.g = rgb[1]; info.b = rgb[2];
    info.found = true;
    return info;
}

// Hand-built rather than assembled via boost::property_tree: write_json quotes every leaf
// value as a JSON string regardless of its C++ type (a documented property_tree limitation),
// which the firmware silently ignores instead of rejecting -- it never acts on a message
// with stringified numbers/booleans. Hand-building guarantees real type fidelity.
std::string build_print_start_payload(const std::string &filename, const std::string &md5, size_t filesize,
                                       const PaintInfo &paint, const PrintTaskOptions &options)
{
    // gcode's own "; paint_info" is 0-based; the payload's paint_index is 1-based. ams_index
    // falls back to paint_index only when no tray was confirmed by the user.
    int payload_paint_index = paint.paint_index + 1;
    int ams_index            = paint.ams_index >= 0 ? paint.ams_index : paint.paint_index;
    // msgid/timestamp are a fixed pair on every real print/start capture, not random -- see
    // ANYCUBIC_INTEGRATION_NOTES.md.
    std::ostringstream j;
    j << "{"
      << "\"type\":\"print\","
      << "\"action\":\"start\","
      << "\"msgid\":\"02fd3987-a2ff-244e-7c95-7fe257a9ef70\","
      << "\"timestamp\":1660201929871,"
      << "\"data\":{"
        << "\"taskid\":\"-1\","
        << "\"url\":\"https://anycubic.com/store/aaa.gcode\","
        << "\"filename\":\"" << json_escape(filename) << "\","
        << "\"md5\":\"" << md5 << "\","
        << "\"filepath\":null,"
        << "\"filetype\":1,"
        << "\"project_type\":1,"
        << "\"filesize\":" << filesize << ","
        << "\"ams_settings\":{"
          << "\"use_ams\":true,\"padding1\":0,\"padding2\":0,\"padding3\":0,"
          << "\"ams_box_mapping\":[{"
            << "\"paint_index\":" << payload_paint_index << ",\"ams_index\":" << ams_index << ","
            << "\"paint_color\":[1,1,1,255],"
            << "\"ams_color\":[" << paint.ams_r << "," << paint.ams_g << "," << paint.ams_b << ",255],"
            << "\"material_type\":\"" << json_escape(paint.material_type) << "\""
          << "}]"
        << "},"
        << "\"task_settings\":{"
          // Real per-print checkboxes on Slicer Next's own Start Print dialog, not constants.
          << "\"auto_leveling\":" << (options.auto_leveling ? 1 : 0) << ","
          << "\"vibration_compensation\":" << (options.vibration_compensation ? 1 : 0) << ","
          << "\"flow_calibration\":" << (options.flow_calibration ? 1 : 0) << ",\"dry_mode\":0,"
          << "\"ai_settings\":{\"status\":0,\"count\":32767,\"type\":32,\"mode\":0},"
          << "\"timelapse\":{\"status\":" << (options.timelapse ? 1 : 0) << ",\"count\":32767,\"type\":32,\"mode\":0},"
          << "\"drying_settings\":{\"status\":0,\"target_temp\":0,\"duration\":0,\"remain_time\":0},"
          << "\"model_objects_skip_parts\":[]"
        << "}"
      << "}"
      << "}";
    return j.str();
}

// Parses a multiColorBox/report JSON body into every tray it reports (not just one), for
// query_trays() to hand to the real confirmation dialog.
std::vector<AceTray> parse_multi_color_box_report(const std::string &json_body)
{
    std::vector<AceTray> trays;
    try {
        std::stringstream ss(json_body);
        pt::ptree root;
        pt::read_json(ss, root);
        auto boxes = root.get_child_optional("data.multi_color_box");
        if (!boxes) return trays;
        for (const auto &box_pair : *boxes) {
            auto slots = box_pair.second.get_child_optional("slots");
            if (!slots) continue;
            for (const auto &slot_pair : *slots) {
                const auto &slot = slot_pair.second;
                AceTray t;
                t.index         = slot.get<int>("index", -1);
                t.material_type = slot.get<std::string>("type", "PLA");
                t.sku           = slot.get<std::string>("sku", "");
                int i = 0;
                if (auto colour = slot.get_child_optional("color")) {
                    for (const auto &c : *colour) {
                        int v = c.second.get_value<int>();
                        if (i == 0) t.r = v; else if (i == 1) t.g = v; else if (i == 2) t.b = v;
                        ++i;
                    }
                }
                if (t.index >= 0)
                    trays.push_back(t);
            }
        }
    } catch (const std::exception &) {
        // malformed/unexpected response shape -- return whatever was parsed so far
    }
    return trays;
}

// Shared connect+CONNACK-check, used by both query_trays() and send() below -- neither needs
// more than one short-lived connection per call, so this isn't kept open between them.
bool mqtt_connect(const LanCredentials &creds, const std::string &client_id,
                   asio::io_context &io, asio::ssl::context &ctx,
                   asio::ssl::stream<asio::ip::tcp::socket> &stream, std::string &out_error)
{
    if (!configure_client_cert(ctx, creds.device_crt, creds.device_pk, out_error))
        return false;
    asio::ip::tcp::resolver resolver(io);
    auto endpoints = resolver.resolve(creds.broker_host, std::to_string(creds.broker_port));
    asio::connect(stream.next_layer(), endpoints);
    stream.handshake(asio::ssl::stream_base::client);

    asio::write(stream, asio::buffer(build_connect_packet(client_id, creds.username, creds.password)));
    std::array<char, 4> connack{};
    asio::read(stream, asio::buffer(connack));
    if (static_cast<unsigned char>(connack[0]) != 0x20 || connack[3] != 0) {
        out_error = (boost::format("MQTT broker rejected CONNECT (return code %1%)") % static_cast<int>(connack[3])).str();
        return false;
    }
    return true;
}

void mqtt_disconnect(asio::ssl::stream<asio::ip::tcp::socket> &stream)
{
    std::string disconnect_packet;
    disconnect_packet.push_back(static_cast<char>(0xE0));
    disconnect_packet.push_back(static_cast<char>(0x00));
    boost::system::error_code ec;
    asio::write(stream, asio::buffer(disconnect_packet), ec);
    stream.shutdown(ec);
    stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    stream.next_layer().close(ec);
}

} // namespace

struct AnycubicMqttSession::Impl
{
    asio::io_context io;
    asio::ssl::context ctx{asio::ssl::context::tls_client};
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket>> stream;
    LanCredentials creds;
    bool connected = false;
};

AnycubicMqttSession::AnycubicMqttSession() : m_impl(new Impl()) {}

AnycubicMqttSession::~AnycubicMqttSession()
{
    disconnect();
}

bool AnycubicMqttSession::connect(const std::string &printer_host, std::string &out_error)
{
    if (!discover_credentials(printer_host, m_impl->creds, out_error))
        return false;

    try {
        m_impl->stream.reset(new asio::ssl::stream<asio::ip::tcp::socket>(m_impl->io, m_impl->ctx));
        if (!mqtt_connect(m_impl->creds, "KobraSlicer-" + random_alphanumeric(8),
                           m_impl->io, m_impl->ctx, *m_impl->stream, out_error))
            return false;
        m_impl->connected = true;
        return true;
    } catch (const std::exception &ex) {
        out_error = std::string("MQTT connection failed: ") + ex.what();
        return false;
    }
}

void AnycubicMqttSession::send_pre_print_check()
{
    if (!m_impl->connected) return;
    const auto &creds = m_impl->creds;
    try {
        const std::string topic = (boost::format("anycubic/anycubicCloud/v1/web/printer/%1%/%2%/lastWill")
                                    % creds.model_id % creds.device_id).str();
        std::ostringstream q;
        q << "{\"type\":\"lastWill\",\"action\":\"query\",\"timestamp\":" << unix_millis_now()
          << ",\"msgid\":\"" << random_msgid() << "\",\"data\":null}";
        asio::write(*m_impl->stream, asio::buffer(build_publish_packet(topic, q.str())));
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "AnycubicMqttSession: pre-print lastWill check failed (non-fatal): " << ex.what();
    }
}

void AnycubicMqttSession::send_startup_queries(const std::string &filename, const std::string &source_file_path,
                                                size_t filesize, const PrintTaskOptions &options)
{
    if (!m_impl->connected) return;
    auto &stream = *m_impl->stream;
    const auto &creds = m_impl->creds;

    // See PaintInfo::real_gcode_size. Falls back to the archive's own disk size if the
    // internal gcode couldn't be read at all.
    size_t gcode_size = filesize;
    {
        PaintInfo pi = extract_paint_info(source_file_path);
        if (pi.real_gcode_size > 0)
            gcode_size = pi.real_gcode_size;
    }

    auto publish_to = [&](const std::string &full_topic, const std::string &json) {
        try {
            asio::write(stream, asio::buffer(build_publish_packet(full_topic, json)));
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(warning) << "AnycubicMqttSession: startup query on " << full_topic
                                        << " failed (non-fatal): " << ex.what();
        }
    };
    auto publish = [&](const std::string &action_topic, const std::string &json) {
        publish_to((boost::format("anycubic/anycubicCloud/v1/web/printer/%1%/%2%/%3%")
                     % creds.model_id % creds.device_id % action_topic).str(), json);
    };

    auto msg = [](const std::string &type, const std::string &action, const std::string &data) {
        return (boost::format("{\"type\":\"%1%\",\"action\":\"%2%\",\"timestamp\":%3%,\"msgid\":\"%4%\",\"data\":%5%}")
                % type % action % unix_millis_now() % random_msgid() % data).str();
    };

    publish("lastWill", msg("lastWill", "query", "null"));
    publish("print", msg("print", "query", "null"));
    publish("calibration", msg("calibration", "getInfo", "null"));
    publish("properties", msg("properties", "read",
        "[\"assistant_settings\",\"hardware_settings\",\"hotbed_settings\",\"option_settings\","
        "\"head_channel_count\",\"camera\"]"));
    publish("extfilbox", msg("extfilbox", "getInfo", "null"));
    publish("extrudeControl", msg("extrudeControl", "getInfo", "null"));

    // "buried" is a real vendor analytics/telemetry event, published straight to the report
    // topic (not a query). estimate_duration/weight, per-tray is_rfid, total_layers and
    // wifi_signal aren't available to this class here, so those are honest placeholders, not
    // fabricated telemetry. cn_code is a fixed device-side constant, confirmed identical across
    // every real capture. app_version/slicer/source_info report this app's own real identity.
    {
        const std::string buried_topic = (boost::format("anycubic/anycubicCloud/v1/printer/public/%1%/%2%/buried/report")
                                           % creds.model_id % creds.device_id).str();
        std::ostringstream b;
        b << "{\"type\":\"buried\",\"action\":\"PrintStart\",\"msgid\":\"" << random_msgid()
          << "\",\"timestamp\":" << unix_millis_now() << ",\"data\":{"
          << "\"app_version\":\"KobraSlicer\","
          << "\"bed_leveling\":" << (options.auto_leveling ? "true" : "false") << ","
          << "\"cn_code\":\"BB0E-A2CA-8F6A-0149\","
          << "\"estimate_duration\":0,\"estimate_weight\":0,"
          << "\"flow_calibration\":" << (options.flow_calibration ? "true" : "false") << ","
          << "\"foreign_object_detection\":false,"
          << "\"gcode_size\":" << gcode_size << ","
          << "\"is_rfid\":[0,0,0,0],"
          << "\"print_filaments\":\"PLA\",\"print_filaments_weight\":\"\","
          << "\"printer_type\":\"Anycubic Kobra 3 Max\","
          << "\"slice_filaments\":\"PLA;PLA;PLA;PLA\","
          << "\"slicer\":\"KobraSlicer\","
          << "\"source_info\":{\"plate_index\":1,\"models_from\":0,"
          << "\"software_version\":\"KobraSlicer\",\"slice_paras_process\":1,"
          << "\"models\":[{\"name\":\"" << json_escape(filename) << "\",\"file_source\":0,\"mo_file_id\":-1}]},"
          << "\"spaghetti_detection\":false,"
          << "\"task_name\":\"" << json_escape(filename) << "\","
          << "\"taskid\":\"-1\","
          << "\"time_lapse\":" << (options.timelapse ? "true" : "false") << ","
          << "\"total_layers\":0,"
          << "\"wifi_signal\":0"
          << "}}";
        publish_to(buried_topic, b.str());
    }
    publish("info", msg("info", "net", "{}"));
    publish("print", msg("print", "getSliceParam", "{\"taskid\":\"-1\"}"));
}

bool AnycubicMqttSession::query_trays(std::vector<AceTray> &out_trays, std::string &out_error)
{
    if (!m_impl->connected) { out_error = "Not connected"; return false; }
    auto &stream = *m_impl->stream;
    const auto &creds = m_impl->creds;

    try {
        const std::string report_topic = (boost::format("anycubic/anycubicCloud/v1/printer/public/%1%/%2%/multiColorBox/report")
                                           % creds.model_id % creds.device_id).str();
        const std::string query_topic  = (boost::format("anycubic/anycubicCloud/v1/web/printer/%1%/%2%/multiColorBox")
                                           % creds.model_id % creds.device_id).str();

        asio::write(stream, asio::buffer(build_subscribe_packet(1, report_topic)));
        uint8_t type = 0; std::string body;
        if (!read_one_mqtt_packet(stream, 3000, type, body) || type != 9 /*SUBACK*/) {
            out_error = "No SUBACK from the printer for the ACE Pro status topic";
            return false;
        }

        std::ostringstream q;
        q << "{\"type\":\"multiColorBox\",\"action\":\"getInfo\",\"timestamp\":" << unix_millis_now()
          << ",\"msgid\":\"" << random_msgid() << "\",\"data\":null}";
        asio::write(stream, asio::buffer(build_publish_packet(query_topic, q.str())));

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!read_one_mqtt_packet(stream, 1000, type, body))
                continue; // per-read timeout, keep trying until the overall deadline
            if (type != 3 /*PUBLISH*/ || body.size() < 2)
                continue;
            uint16_t topic_len = (static_cast<unsigned char>(body[0]) << 8) | static_cast<unsigned char>(body[1]);
            if (body.size() < 2u + topic_len)
                continue;
            std::string topic = body.substr(2, topic_len);
            if (topic != report_topic)
                continue;
            out_trays = parse_multi_color_box_report(body.substr(2 + topic_len));
            if (!out_trays.empty())
                break;
        }

        // Best-effort cleanup -- not treated as a failure if it doesn't go through.
        asio::write(stream, asio::buffer(build_unsubscribe_packet(1, report_topic)));

        if (out_trays.empty()) {
            out_error = "The printer never answered the ACE Pro tray query in time";
            return false;
        }
        return true;
    } catch (const std::exception &ex) {
        out_error = std::string("MQTT query failed: ") + ex.what();
        return false;
    }
}

bool AnycubicMqttSession::query_task_settings(PrintTaskOptions &out_options, std::string &out_error)
{
    if (!m_impl->connected) { out_error = "Not connected"; return false; }
    auto &stream = *m_impl->stream;
    const auto &creds = m_impl->creds;

    try {
        // getTaskSettings shares the "print"/"print/report" topic pair with getSliceParam.
        const std::string report_topic = (boost::format("anycubic/anycubicCloud/v1/printer/public/%1%/%2%/print/report")
                                           % creds.model_id % creds.device_id).str();
        const std::string query_topic  = (boost::format("anycubic/anycubicCloud/v1/web/printer/%1%/%2%/print")
                                           % creds.model_id % creds.device_id).str();

        asio::write(stream, asio::buffer(build_subscribe_packet(4, report_topic)));
        uint8_t type = 0; std::string body;
        if (!read_one_mqtt_packet(stream, 3000, type, body) || type != 9 /*SUBACK*/) {
            out_error = "No SUBACK from the printer for the print/report topic";
            return false;
        }

        std::ostringstream q;
        q << "{\"type\":\"print\",\"action\":\"getTaskSettings\",\"timestamp\":" << unix_millis_now()
          << ",\"msgid\":\"" << random_msgid() << "\",\"data\":{\"taskid\":\"-1\"}}";
        asio::write(stream, asio::buffer(build_publish_packet(query_topic, q.str())));

        bool found = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!read_one_mqtt_packet(stream, 1000, type, body))
                continue;
            if (type != 3 /*PUBLISH*/ || body.size() < 2)
                continue;
            uint16_t topic_len = (static_cast<unsigned char>(body[0]) << 8) | static_cast<unsigned char>(body[1]);
            if (body.size() < 2u + topic_len)
                continue;
            if (body.substr(2, topic_len) != report_topic)
                continue;
            std::string json_body = body.substr(2 + topic_len);
            try {
                std::stringstream ss(json_body);
                pt::ptree root;
                pt::read_json(ss, root);
                if (root.get<std::string>("action", "") != "getTaskSettings")
                    continue; // a getSliceParam report landed on the same topic -- keep waiting
                auto settings = root.get_child_optional("data.task_settings");
                if (!settings)
                    continue;
                out_options.auto_leveling          = settings->get<int>("auto_leveling", out_options.auto_leveling ? 1 : 0) != 0;
                out_options.vibration_compensation = settings->get<int>("vibration_compensation", out_options.vibration_compensation ? 1 : 0) != 0;
                out_options.flow_calibration       = settings->get<int>("flow_calibration", out_options.flow_calibration ? 1 : 0) != 0;
                out_options.timelapse              = settings->get<int>("timelapse.status", out_options.timelapse ? 1 : 0) != 0;
                found = true;
                break;
            } catch (const std::exception &) {
                // not the response we're waiting for, or a shape we don't recognise -- keep waiting
            }
        }

        if (!found) {
            out_error = "The printer never answered the task-settings query in time -- using defaults";
            return false;
        }
        return true;
    } catch (const std::exception &ex) {
        out_error = std::string("MQTT task-settings query failed: ") + ex.what();
        return false;
    }
}

bool AnycubicMqttSession::verify_uploaded_file(const std::string &filename, std::string &out_error)
{
    if (!m_impl->connected) { out_error = "Not connected"; return false; }
    auto &stream = *m_impl->stream;
    const auto &creds = m_impl->creds;

    try {
        const std::string report_topic = (boost::format("anycubic/anycubicCloud/v1/printer/public/%1%/%2%/file/report")
                                           % creds.model_id % creds.device_id).str();
        const std::string query_topic  = (boost::format("anycubic/anycubicCloud/v1/web/printer/%1%/%2%/file")
                                           % creds.model_id % creds.device_id).str();

        asio::write(stream, asio::buffer(build_subscribe_packet(2, report_topic)));
        uint8_t type = 0; std::string body;
        if (!read_one_mqtt_packet(stream, 3000, type, body) || type != 9 /*SUBACK*/) {
            out_error = "No SUBACK from the printer for the file-details topic";
            return false;
        }

        // "local" is the printer's own local storage, where the HTTP upload lands (vs "udisk"
        // for a USB stick).
        std::ostringstream q;
        q << "{\"type\":\"file\",\"action\":\"fileDetails\",\"timestamp\":" << unix_millis_now()
          << ",\"msgid\":\"" << random_msgid() << "\",\"data\":{\"root\":\"local\",\"filename\":\""
          << json_escape(filename) << "\"}}";
        asio::write(stream, asio::buffer(build_publish_packet(query_topic, q.str())));

        bool found = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!read_one_mqtt_packet(stream, 1000, type, body))
                continue;
            if (type != 3 /*PUBLISH*/ || body.size() < 2)
                continue;
            uint16_t topic_len = (static_cast<unsigned char>(body[0]) << 8) | static_cast<unsigned char>(body[1]);
            if (body.size() < 2u + topic_len)
                continue;
            if (body.substr(2, topic_len) != report_topic)
                continue;
            std::string json_body = body.substr(2 + topic_len);
            try {
                std::stringstream ss(json_body);
                pt::ptree root;
                pt::read_json(ss, root);
                if (root.get_child_optional("data.file_details")) {
                    found = true;
                    break;
                }
            } catch (const std::exception &) {
                // not the response we're waiting for, or a shape we don't recognise -- keep waiting
            }
        }

        if (!found) {
            out_error = "The printer never confirmed the uploaded file exists (no file_details in time)";
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "AnycubicMqttSession: printer confirmed " << filename << " is present";
        return true;
    } catch (const std::exception &ex) {
        out_error = std::string("MQTT file verification failed: ") + ex.what();
        return false;
    }
}

bool AnycubicMqttSession::send_print_start(const std::string &uploaded_filename, const std::string &source_file_path,
                                            const AceTray *chosen_tray, const PrintTaskOptions &options,
                                            std::string &out_error)
{
    if (!m_impl->connected) { out_error = "Not connected"; return false; }

    std::string file_md5;
    if (!md5_hex_file(source_file_path, file_md5)) {
        out_error = "Could not read the uploaded file to compute its MD5";
        return false;
    }
    size_t file_size = 0;
    try { file_size = static_cast<size_t>(fs::file_size(source_file_path)); }
    catch (const std::exception &ex) { out_error = std::string("Could not get file size: ") + ex.what(); return false; }

    // paint_index/paint_color stay as the file's own real values; only ams_index/ams_r/g/b
    // come from the chosen tray. material_type reflects the chosen tray -- see PaintInfo.
    PaintInfo effective = extract_paint_info(source_file_path);
    if (chosen_tray) {
        effective.material_type = chosen_tray->material_type;
        effective.ams_index     = chosen_tray->index;
        effective.ams_r = chosen_tray->r; effective.ams_g = chosen_tray->g; effective.ams_b = chosen_tray->b;
        effective.found         = true;
    } else if (!effective.found) {
        BOOST_LOG_TRIVIAL(warning) << "AnycubicMqttSession: no confirmed tray and no readable gcode "
            "paint_info -- falling back to a default AMS mapping (tray 1, white, PLA).";
    }

    const auto &creds = m_impl->creds;
    auto print_topic = (boost::format("anycubic/anycubicCloud/v1/slicer/printer/%1%/%2%/print")
                         % creds.model_id % creds.device_id).str();
    auto payload = build_print_start_payload(uploaded_filename, file_md5, file_size, effective, options);

    // Subscribes to print/report and waits for the real response (multiplexed by "action",
    // same as query_task_settings()) rather than firing and forgetting -- the printer's actual
    // rejection reason is only visible this way, not from the MQTT accept alone.
    const std::string report_topic = (boost::format("anycubic/anycubicCloud/v1/printer/public/%1%/%2%/print/report")
                                       % creds.model_id % creds.device_id).str();
    try {
        asio::write(*m_impl->stream, asio::buffer(build_subscribe_packet(5, report_topic)));
        uint8_t type = 0; std::string body;
        if (!read_one_mqtt_packet(*m_impl->stream, 3000, type, body) || type != 9 /*SUBACK*/) {
            out_error = "No SUBACK from the printer for the print/report topic -- sending anyway, blind";
            BOOST_LOG_TRIVIAL(warning) << "AnycubicMqttSession: " << out_error;
        }
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "AnycubicMqttSession: could not subscribe before print/start (non-fatal, sending blind): " << ex.what();
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "AnycubicMqttSession: publishing print/start on " << print_topic
            << " (same connection used for the tray query)";
        asio::write(*m_impl->stream, asio::buffer(build_publish_packet(print_topic, payload)));
    } catch (const std::exception &ex) {
        out_error = std::string("MQTT publish failed: ") + ex.what();
        return false;
    }

    uint8_t type = 0; std::string body;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!read_one_mqtt_packet(*m_impl->stream, 1000, type, body))
            continue;
        if (type != 3 /*PUBLISH*/ || body.size() < 2)
            continue;
        uint16_t topic_len = (static_cast<unsigned char>(body[0]) << 8) | static_cast<unsigned char>(body[1]);
        if (body.size() < 2u + topic_len || body.substr(2, topic_len) != report_topic)
            continue;
        std::string json_body = body.substr(2 + topic_len);
        try {
            std::stringstream ss(json_body);
            pt::ptree root;
            pt::read_json(ss, root);
            if (root.get<std::string>("action", "") != "start")
                continue; // a getSliceParam/getTaskSettings report landed on the same topic
            std::string state = root.get<std::string>("state", "");
            if (state == "failed") {
                std::string real_msg = root.get<std::string>("msg", "");
                out_error = "Printer rejected the print: " + real_msg;
                BOOST_LOG_TRIVIAL(error) << "AnycubicMqttSession: real print/start rejection: " << real_msg;
                return false;
            }
            BOOST_LOG_TRIVIAL(info) << "AnycubicMqttSession: print/start accepted, printer state: " << state;
            return true;
        } catch (const std::exception &) {
            // not the response we're waiting for, or a shape we don't recognise -- keep waiting
        }
    }

    // No response in time either way -- match the old fire-and-forget behaviour (assume it
    // worked) rather than fail a print that may well be fine, but log it clearly since it means
    // we genuinely don't know.
    BOOST_LOG_TRIVIAL(warning) << "AnycubicMqttSession: no print/start response in time -- proceeding without confirmation";
    return true;
}

void AnycubicMqttSession::disconnect()
{
    if (m_impl->connected && m_impl->stream) {
        mqtt_disconnect(*m_impl->stream);
        m_impl->connected = false;
    }
}

bool AnycubicMqttSession::read_expected_tray(const std::string &source_file_path,
                                              int &out_index_0based, std::string &out_material_type)
{
    PaintInfo paint = extract_paint_info(source_file_path);
    if (!paint.found)
        return false;
    out_index_0based  = paint.paint_index;
    out_material_type = paint.material_type;
    return true;
}

}

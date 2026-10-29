#include <cstring>
#include <vector>

#include "SimRig.h"
#include "hc/api/ApiRouter.h"
#include "hc/system/Firmware.h"

using namespace hc;
using sim::Rig;

namespace {

// Synthetic ESP32-S3 application image: image header (magic 0xE9, chip id
// 0x0009), first segment header, esp_app_desc_t magic, filler, marker.
std::vector<uint8_t> makeImage(uint16_t chip, bool appDesc, const std::string& marker, size_t size = 4096) {
  std::vector<uint8_t> img(size);
  for (size_t i = 0; i < size; ++i) img[i] = static_cast<uint8_t>((i * 131u + 7u) & 0xFF);
  img[0] = 0xE9;
  img[12] = static_cast<uint8_t>(chip & 0xFF);
  img[13] = static_cast<uint8_t>(chip >> 8);
  uint32_t m = appDesc ? 0xABCD5432u : 0u;
  std::memcpy(&img[32], &m, 4);
  // a decoy: the bare prefix as it might appear in code, not followed by a version
  const char decoy[] = "HCFW1|KC868-A16V3|";
  std::memcpy(&img[600], decoy, sizeof(decoy));
  if (!marker.empty()) std::memcpy(&img[2000], marker.c_str(), marker.size() + 1);
  return img;
}

std::string sha(const std::vector<uint8_t>& d) {
  Sha256 s;
  s.update(d.data(), d.size());
  return s.hex();
}

}  // namespace

TEST(firmware_sha256_vectors) {
  Sha256 a;
  CHECK_STR(a.hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  Sha256 b;
  b.update(reinterpret_cast<const uint8_t*>("abc"), 3);
  CHECK_STR(b.hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // two-block message, fed byte by byte
  const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  Sha256 c;
  for (size_t i = 0; m[i]; ++i) c.update(reinterpret_cast<const uint8_t*>(m + i), 1);
  CHECK_STR(c.hex(), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(firmware_version_compare) {
  CHECK(compareVersions("0.5.0", "0.4.9") > 0);
  CHECK(compareVersions("v1.2.3", "1.2.3") == 0);
  CHECK(compareVersions("1.10.0", "1.9.9") > 0);
  CHECK(compareVersions("0.5.0-rc1", "0.5.0") < 0);
  CHECK(compareVersions("0.5.0-dev+1a2b3c", "0.5.0-dev") == 0);
  CHECK(compareVersions("0.5.1-dev", "0.5.0") > 0);
  CHECK(compareVersions("1.0", "1.0.0") == 0);
}

TEST(firmware_own_marker_and_version) {
  // the version string is the tail of the image marker
  std::string marker = std::string("HCFW1|") + kBoardId + "|" + kFirmwareVersion;
  std::vector<uint8_t> img = makeImage(FirmwareImageCheck::kChipEsp32S3, true, marker);
  FirmwareImageCheck chk;
  CHECK(chk.feed(img.data(), img.size()));
  std::string err;
  CHECK(chk.finish(err));
  CHECK_STR(chk.version(), kFirmwareVersion);
}

TEST(firmware_image_accepts_valid_chunked_upload) {
  std::vector<uint8_t> img = makeImage(0x0009, true, "HCFW1|KC868-A16V3|1.2.3", 70000);
  // chunk sizes that split header, decoy and marker at awkward places
  for (size_t chunk : {1u, 7u, 47u, 1436u, 70000u}) {
    FirmwareImageCheck chk;
    bool ok = true;
    for (size_t off = 0; off < img.size(); off += chunk)
      ok = ok && chk.feed(img.data() + off, std::min(chunk, img.size() - off));
    CHECK(ok);
    std::string err;
    CHECK(chk.finish(err, sha(img), img.size()));
    CHECK_STR(err, "");
    CHECK_STR(chk.version(), "1.2.3");
    CHECK_STR(chk.sha256(), sha(img));
  }
}

TEST(firmware_image_rejections) {
  std::string err;
  {  // ESP32 (classic) image
    auto img = makeImage(0x0000, true, "HCFW1|KC868-A16V3|1.0.0");
    FirmwareImageCheck chk;
    CHECK(!chk.feed(img.data(), img.size()));
    CHECK(!chk.finish(err));
    CHECK(err.find("ESP32-S3") != std::string::npos);
  }
  {  // merged factory image starts with the bootloader (no app descriptor)
    auto img = makeImage(0x0009, false, "HCFW1|KC868-A16V3|1.0.0");
    FirmwareImageCheck chk;
    CHECK(!chk.feed(img.data(), img.size()));
    CHECK(!chk.finish(err));
    CHECK(err.find("Factory") != std::string::npos);
  }
  {  // not a file at all
    std::vector<uint8_t> txt(100, 'x');
    FirmwareImageCheck chk;
    CHECK(!chk.feed(txt.data(), txt.size()));
  }
  {  // other ESP32-S3 firmware (no HeatCore marker; decoy prefix only)
    auto img = makeImage(0x0009, true, "");
    FirmwareImageCheck chk;
    CHECK(chk.feed(img.data(), img.size()));
    CHECK(!chk.finish(err));
    CHECK(err.find("keine HeatCore") != std::string::npos);
  }
  {  // HeatCore for another board
    auto img = makeImage(0x0009, true, "HCFW1|KC868-A8V3|1.0.0");
    FirmwareImageCheck chk;
    CHECK(chk.feed(img.data(), img.size()));
    CHECK(!chk.finish(err));
  }
  {  // corrupted in transit: checksum and size are checked
    auto img = makeImage(0x0009, true, "HCFW1|KC868-A16V3|1.0.0");
    std::string good = sha(img);
    img[3000] ^= 0x01;
    FirmwareImageCheck chk;
    CHECK(chk.feed(img.data(), img.size()));
    CHECK(!chk.finish(err, good));
    CHECK(err.find("SHA-256") != std::string::npos);
    FirmwareImageCheck chk2;
    chk2.feed(img.data(), img.size() - 10);
    CHECK(!chk2.finish(err, std::string(), img.size()));
  }
  {  // too small
    uint8_t b[10] = {0xE9};
    FirmwareImageCheck chk;
    chk.feed(b, sizeof(b));
    CHECK(!chk.finish(err));
  }
}

TEST(api_firmware_info_and_update_url) {
  Rig r;
  ApiRouter api(r.sys);
  r.sys.platform.firmware.runningPartition = "app0";
  r.sys.platform.firmware.rollbackAvailable = true;
  ApiRequest q;
  q.method = "GET";
  q.path = "/api/firmware";
  ApiResponse res = api.handle(q);
  CHECK_EQ(res.status, 200);
  JsonDocument d;
  deserializeJson(d, res.body);
  CHECK_STR(d["version"] | "", kFirmwareVersion);
  CHECK_STR(d["board"] | "", "KC868-A16V3");
  CHECK_STR(d["runningPartition"] | "", "app0");
  CHECK(d["rollbackAvailable"] | false);
  CHECK(std::string(d["updateUrl"] | "").rfind("https://", 0) == 0);
  // rollback needs the platform
  q.method = "POST";
  q.path = "/api/firmware/rollback";
  CHECK_EQ(api.handle(q).status, 501);
  std::string seen;
  r.sys.platformCommand = [&](const std::string& cmd, JsonObjectConst) {
    seen = cmd;
    return std::string();
  };
  CHECK_EQ(api.handle(q).status, 200);
  CHECK_STR(seen, "ota_rollback");
  // update source: https only, trailing slash added, empty allowed
  r.load(ConfigSection::System, R"({"updateUrl":"https://example.org/hc"})");
  CHECK_STR(r.sys.sysCfg.updateUrl, "https://example.org/hc/");
  JsonDocument bad;
  deserializeJson(bad, R"({"updateUrl":"http://example.org/"})");
  CHECK(!r.sys.sectionFromJson(ConfigSection::System, bad.as<JsonObjectConst>()).empty());
  r.load(ConfigSection::System, R"({"updateUrl":""})");
  CHECK_STR(r.sys.sysCfg.updateUrl, "");
}

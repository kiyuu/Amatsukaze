/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "AMTLogo.h"
#include "rgy_util.h"
#include "AviSynthWrapper.h"
#include "ReaderWriterFFmpeg.h"
extern "C" {
#include <libavutil/sha.h>
}

logo::LogoHeader::LogoHeader() {}

logo::LogoHeader::LogoHeader(int w, int h, int logUVx, int logUVy, int imgw, int imgh, int imgx, int imgy, const std::string& name)
    : magic(LOGO_EXTENDED_MAGIC)
    , version(LOGO_EXTENDED_VERSION)
    , w(w)
    , h(h)
    , logUVx(logUVx)
    , logUVy(logUVy)
    , imgw(imgw)
    , imgh(imgh)
    , imgx(imgx)
    , imgy(imgy)
    , name()
    , reserved() {
    strncpy_s(this->name, name.c_str(), sizeof(this->name) - 1);
}

/* static */ void logo::LogoData::ToYC48Y(float& y) {
    y = float(((int(y * 255) * 1197) >> 6) - 299);
}

/* static */ void logo::LogoData::ToYC48C(float& u) {
    u = float(((int(u * 255) - 128) * 4681 + 164) >> 8);
}

/* static */ void logo::LogoData::ToYV12Y(float& y) {
    y = float(((((int)y * 219 + 383) >> 12) + 16) / 255.0f);
}

/* static */ void logo::LogoData::ToYV12C(float& u) {
    u = float((((((int)u + 2048) * 7 + 66) >> 7) + 16) / 255.0f);
}

/* static */ void logo::LogoData::ToYC48ABY(float& A, float& B) {
    float x0 = 0, x1 = 2048;
    ToYV12Y(x0); ToYV12Y(x1);
    float y0 = (x0 - B) / A;
    float y1 = (x1 - B) / A;
    ToYC48Y(y0); ToYC48Y(y1);
    // (0,y0),(2048,y1)を通る直線
    B = y0;
    A = (y1 - y0) / 2048.0f;
}

/* static */ void logo::LogoData::ToYC48ABC(float& A, float& B) {
    float x0 = 0, x1 = 2048;
    ToYV12C(x0); ToYV12C(x1);
    float y0 = (x0 - B) / A;
    float y1 = (x1 - B) / A;
    ToYC48C(y0); ToYC48C(y1);
    // (0,y0),(2048,y1)を通る直線
    B = y0;
    A = (y1 - y0) / 2048.0f;
}

/* static */ void logo::LogoData::ToOutLGP(LOGO_PIXEL& lgp, float aY, float bY, float aU, float bU, float aV, float bV) {
    float A;
    float B;
    float temp;

    // 輝度
    A = aY;
    B = bY;
    ToYC48ABY(A, B);
    if (A == 1) {	// 0での除算回避
        lgp.y = lgp.dp_y = 0;
    } else {
        temp = B / (1 - A) + 0.5f;
        if (std::abs(temp) < 0x7FFF) {
            // shortの範囲内
            lgp.y = (short)temp;
            temp = (1 - A) * LOGO_MAX_DP + 0.5f;
            if (std::abs(temp) > 0x3FFF || short(temp) == 0)
                lgp.y = lgp.dp_y = 0;
            else
                lgp.dp_y = (short)temp;
        } else
            lgp.y = lgp.dp_y = 0;
    }

    // 色差(青)
    A = aU;
    B = bU;
    ToYC48ABC(A, B);
    if (A == 1) {
        lgp.cb = lgp.dp_cb = 0;
    } else {
        temp = B / (1 - A) + 0.5f;
        if (std::abs(temp) < 0x7FFF) {
            // short範囲内
            lgp.cb = (short)temp;
            temp = (1 - A) * LOGO_MAX_DP + 0.5f;
            if (std::abs(temp) > 0x3FFF || short(temp) == 0)
                lgp.cb = lgp.dp_cb = 0;
            else
                lgp.dp_cb = (short)temp;
        } else
            lgp.cb = lgp.dp_cb = 0;
    }

    // 色差(赤)
    A = aV;
    B = bV;
    ToYC48ABC(A, B);
    if (A == 1) {
        lgp.cr = lgp.dp_cr = 0;
    } else {
        temp = B / (1 - A) + 0.5f;
        if (std::abs(temp) < 0x7FFF) {
            // short範囲内
            lgp.cr = (short)temp;
            temp = (1 - A) * LOGO_MAX_DP + 0.5f;
            if (std::abs(temp) > 0x3FFF || short(temp) == 0)
                lgp.cr = lgp.dp_cr = 0;
            else
                lgp.dp_cr = (short)temp;
        } else
            lgp.cr = lgp.dp_cr = 0;
    }
}

void logo::LogoData::WriteBaseLogo(File& file, const LogoHeader* header, const LOGO_PIXEL* data) {
    LOGO_FILE_HEADER fh = { 0 };
    strcpy_s(fh.str, LOGO_FILE_HEADER_STR);
    fh.logonum.l = SWAP_ENDIAN(1);
    file.writeValue(fh);

    LOGO_HEADER h = { 0 };
    memcpy(h.name, header->name, std::min(strlen(header->name), sizeof(h.name) - 1));
    h.x = header->imgx;
    h.y = header->imgy;
    h.w = header->w;
    h.h = header->h;
    file.writeValue(h);

    size_t sz = header->w * header->h;
    file.write(MemoryChunk((uint8_t*)data, sz * sizeof(data[0])));
}

void logo::LogoData::WriteExtendedLogo(File& file, const LogoHeader* header) {
    int wUV = w >> logUVx;
    int hUV = h >> logUVy;
    size_t sz = (header->w * header->h + wUV * hUV * 2) * 2;

    file.writeValue(*header);
    file.write(MemoryChunk((uint8_t*)data.get(), sz * sizeof(float)));
}
logo::LogoData::LogoData() {}

logo::LogoData::LogoData(int w, int h, int logUVx, int logUVy)
    : w(w), h(h), logUVx(logUVx), logUVy(logUVy) {
    int wUV = w >> logUVx;
    int hUV = h >> logUVy;
    data = std::unique_ptr<float[]>(new float[(w * h + wUV * hUV * 2) * 2]);
    aY = data.get();
    bY = aY + w * h;
    aU = bY + w * h;
    bU = aU + wUV * hUV;
    aV = bU + wUV * hUV;
    bV = aV + wUV * hUV;
}

bool logo::LogoData::isValid() const { return (data != nullptr); }
int logo::LogoData::getWidth() const { return w; }
int logo::LogoData::getHeight() const { return h; }
int logo::LogoData::getLogUVx() const { return logUVx; }
int logo::LogoData::getLogUVy() const { return logUVy; }

float* logo::LogoData::GetA(int plane) {
    switch (plane) {
    case PLANAR_Y: return aY;
    case PLANAR_U: return aU;
    case PLANAR_V: return aV;
    }
    return nullptr;
}

float* logo::LogoData::GetB(int plane) {
    switch (plane) {
    case PLANAR_Y: return bY;
    case PLANAR_U: return bU;
    case PLANAR_V: return bV;
    }
    return nullptr;
}

void logo::LogoData::Save(const tstring& filepath, const LogoHeader* header) {
    // ベース部分作成
    int wUV = w >> logUVx;
    std::vector<LOGO_PIXEL> basedata(header->w * header->h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int off = x + y * w;
            int offUV = (x >> logUVx) + (y >> logUVy) * wUV;
            ToOutLGP(basedata[off], aY[off], bY[off], aU[offUV], bU[offUV], aV[offUV], bV[offUV]);
        }
    }

    File file(filepath, _T("wb"));
    WriteBaseLogo(file, header, basedata.data());
    WriteExtendedLogo(file, header);
}

void logo::LogoData::SaveAviUtl(const tstring& filepath, const LogoHeader* header) {
    int wUV = w >> logUVx;
    std::vector<LOGO_PIXEL> basedata(header->w * header->h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int off = x + y * w;
            int offUV = (x >> logUVx) + (y >> logUVy) * wUV;
            ToOutLGP(basedata[off], aY[off], bY[off], aU[offUV], bU[offUV], aV[offUV], bV[offUV]);
        }
    }

    File file(filepath, _T("wb"));
    WriteBaseLogo(file, header, basedata.data());
}

/* static */ logo::LogoData logo::LogoData::Load(
    const tstring& filepath, LogoHeader* header, LogoLoadHash* contentHash) {
    File file(filepath, _T("rb"));
    AVSHA* sha = nullptr;
    if (contentHash != nullptr) {
        *contentHash = {};
    }
    bool hashAvailable = contentHash != nullptr;
    if (contentHash != nullptr) {
        sha = av_sha_alloc();
        if (sha == nullptr || av_sha_init(sha, 256) < 0) {
            av_free(sha);
            sha = nullptr;
            hashAvailable = false;
        }
    }
    struct ShaGuard {
        AVSHA* value;
        ~ShaGuard() { av_free(value); }
    } shaGuard{ sha };
    auto hashBytes = [&](const uint8_t* bytes, const size_t byteCount) {
        if (sha == nullptr) return;
        size_t offset = 0;
        constexpr size_t HASH_CHUNK_SIZE = 1024 * 1024;
        while (offset < byteCount) {
            const size_t chunk = std::min(HASH_CHUNK_SIZE, byteCount - offset);
            av_sha_update(sha, bytes + offset, static_cast<unsigned int>(chunk));
            offset += chunk;
        }
    };
    auto readHashedValue = [&](auto& value) {
        if (file.read(MemoryChunk(reinterpret_cast<uint8_t*>(&value), sizeof(value))) != sizeof(value)) {
            THROW(IOException, "ロゴファイルのヘッダが途中で終わっています");
        }
        hashBytes(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
    };

    LOGO_FILE_HEADER fileHeader = {};
    LOGO_HEADER h = {};
    if (sha != nullptr) {
        readHashedValue(fileHeader);
        readHashedValue(h);
        int64_t remainingBaseBytes = LOGO_PIXELSIZE(&h);
        if (remainingBaseBytes < 0) {
            THROW(FormatException, "ロゴファイルのベースデータサイズが不正です");
        }
        std::array<uint8_t, 64 * 1024> skipBuffer = {};
        while (remainingBaseBytes > 0) {
            const size_t chunk = static_cast<size_t>(std::min<int64_t>(remainingBaseBytes, skipBuffer.size()));
            if (file.read(MemoryChunk(skipBuffer.data(), chunk)) != chunk) {
                THROW(IOException, "ロゴファイルのベースデータが途中で終わっています");
            }
            hashBytes(skipBuffer.data(), chunk);
            remainingBaseBytes -= static_cast<int64_t>(chunk);
        }
        readHashedValue(*header);
    } else {
        fileHeader = file.readValue<LOGO_FILE_HEADER>();
        h = file.readValue<LOGO_HEADER>();
        file.seek(LOGO_PIXELSIZE(&h), SEEK_CUR);
        *header = file.readValue<LogoHeader>();
    }

    // 拡張部分の妥当性チェック (別形式や壊れたファイルの値で領域を確保しないように)
    if (header->magic != LOGO_EXTENDED_MAGIC || header->version < LOGO_EXTENDED_VERSION) {
        THROW(FormatException, "ロゴファイルの拡張ヘッダが不正です");
    }
    if (header->w <= 0 || header->h <= 0 || header->logUVx < 0 || header->logUVx > LOGO_MAX_LOG_UV
        || header->logUVy < 0 || header->logUVy > LOGO_MAX_LOG_UV) {
        THROW(FormatException, "ロゴファイルのサイズ情報が不正です");
    }

    LogoData logo(header->w, header->h, header->logUVx, header->logUVy);

    int wUV = header->w >> header->logUVx;
    int hUV = header->h >> header->logUVy;
    size_t sz = (header->w * header->h + wUV * hUV * 2) * 2;

    // 途中で切れたファイルを未初期化の係数で読み込まないよう、読めたサイズを確認する
    const size_t extendedDataBytes = sz * sizeof(float);
    if (file.read(MemoryChunk((uint8_t*)logo.data.get(), extendedDataBytes)) != extendedDataBytes) {
        THROW(IOException, "ロゴファイルの係数データが不足しています");
    }
    hashBytes(reinterpret_cast<const uint8_t*>(logo.data.get()), extendedDataBytes);

    if (sha != nullptr) {
        std::array<uint8_t, 64 * 1024> trailingBuffer = {};
        while (true) {
            const size_t bytesRead = file.read(MemoryChunk(trailingBuffer.data(), trailingBuffer.size()));
            if (bytesRead == 0) break;
            hashBytes(trailingBuffer.data(), bytesRead);
        }
        av_sha_final(sha, contentHash->sha256.data());
        contentHash->available = true;
    } else if (contentHash != nullptr) {
        contentHash->available = hashAvailable;
    }

    return logo;
}

namespace {
// テスト用に連結した平面とLogoDataの平面の対応 (aY, bY, aU, bU, aV, bV)
template <typename Func>
void ForEachLogoPlaneForTest(logo::LogoData& logo, Func func) {
    const size_t sizeY = (size_t)logo.getWidth() * logo.getHeight();
    const size_t sizeUV = (size_t)(logo.getWidth() >> logo.getLogUVx()) * (logo.getHeight() >> logo.getLogUVy());
    func(logo.GetA(PLANAR_Y), sizeY);
    func(logo.GetB(PLANAR_Y), sizeY);
    func(logo.GetA(PLANAR_U), sizeUV);
    func(logo.GetB(PLANAR_U), sizeUV);
    func(logo.GetA(PLANAR_V), sizeUV);
    func(logo.GetB(PLANAR_V), sizeUV);
}
size_t LogoPlaneValueCountForTest(int w, int h, int logUVx, int logUVy) {
    return ((size_t)w * h + (size_t)(w >> logUVx) * (h >> logUVy) * 2) * 2;
}
}

extern "C" void InitLogoHeaderForTest(logo::LogoHeader* header, int w, int h, int logUVx, int logUVy,
    int imgw, int imgh, int imgx, int imgy, const char* name) {
    if (header == nullptr) return;
    new (header) logo::LogoHeader(w, h, logUVx, logUVy, imgw, imgh, imgx, imgy, name ? name : "");
}

extern "C" int SaveLogoForTest(const tchar* path, const logo::LogoHeader* header,
    const float* planes, size_t planeValueCount, int aviUtlOnly) {
    if (path == nullptr || header == nullptr || planes == nullptr
        || planeValueCount != LogoPlaneValueCountForTest(header->w, header->h, header->logUVx, header->logUVy)) {
        return -1;
    }
    try {
        logo::LogoData data(header->w, header->h, header->logUVx, header->logUVy);
        const float* src = planes;
        ForEachLogoPlaneForTest(data, [&](float* dst, size_t count) {
            std::copy(src, src + count, dst);
            src += count;
        });
        if (aviUtlOnly) {
            data.SaveAviUtl(path, header);
        } else {
            data.Save(path, header);
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int LoadLogoForTest(const tchar* path, logo::LogoHeader* header,
    float* planes, size_t planeCapacity, size_t* planeValueCount) {
    if (path == nullptr || header == nullptr || planeValueCount == nullptr) {
        return -1;
    }
    *planeValueCount = 0;
    try {
        auto data = logo::LogoData::Load(path, header);
        *planeValueCount = LogoPlaneValueCountForTest(data.getWidth(), data.getHeight(), data.getLogUVx(), data.getLogUVy());
        if (planes == nullptr || planeCapacity < *planeValueCount) {
            return -2;
        }
        float* dst = planes;
        ForEachLogoPlaneForTest(data, [&](float* src, size_t count) {
            std::copy(src, src + count, dst);
            dst += count;
        });
        return 0;
    } catch (...) {
        *planeValueCount = 0;
        return -1;
    }
}

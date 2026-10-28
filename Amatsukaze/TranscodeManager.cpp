/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "TranscodeManager.h"
#include "rgy_util.h"
#include "rgy_thread_affinity.h"
#include <algorithm>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <exception>
#include "AdtsParser.h"
#include "PacketCache.h"
#include "AudioTrackBuilder.h"
#include "rgy_pipe.h"
#include "rgy_mutex.h"
#include "Subtitle.h"
#include "WaveWriter.h"
#include <cmath>
#include <cstring>
#include <limits>
#include "Mpeg2PartialEncode.h"
#include "CaptionPgs.h"
#include <filesystem>
#include <type_traits>
#include <array>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

// tsreadexの実行ファイルが利用可能かを判定する（ファイル存在 or PATH上に存在）
static bool isTsReadExAvailable(const ConfigWrapper& setting) {
    const auto& path = setting.getTsReadExPath();
    return !path.empty()
        && (rgy_file_exists(path) || !find_executable_in_path(path).empty());
}

struct WhisperAudioEntry {
    int keyIndex;
    EncodeFileKey key;
    int localIndex;
    tstring audioPath;
    int audioSourceIndex;
    int dualMonoChannel; // -1: original stereo, 0/1: dual mono channel selection
};

constexpr int RESUME_MANIFEST_VERSION = 5; // --resume-dirとResume.datで共有する形式番号
// 前回の解析結果を再利用できるように保存する。
constexpr int64_t MAX_RESUME_DAT_PAYLOAD_SIZE = 64LL * 1024 * 1024;
constexpr uint32_t MAX_RESUME_DAT_LOGO_COUNT = 4096;
constexpr uint32_t MAX_RESUME_DAT_VIDEO_COUNT = 4096;
constexpr uint32_t MAX_RESUME_DAT_LOGO_TABLE_COUNT =
    MAX_RESUME_DAT_LOGO_COUNT * 2 + MAX_RESUME_DAT_VIDEO_COUNT;
constexpr uint32_t MAX_RESUME_DAT_STRING_SIZE = 1024 * 1024;
constexpr int64_t RESUME_DAT_VIDEO_RECORD_MIN_SIZE = sizeof(int32_t) * 2 + sizeof(uint32_t) + sizeof(uint64_t);

struct ResumeDatLogoEntry {
    tstring filename;
    std::array<uint8_t, 32> sha256 = {};
};

struct ResumeDatScanRange {
    int32_t start = 0;
    int32_t endExclusive = 0;
};

struct ResumeDatVideoInfo {
    int numFrames = 0;
    int32_t selectedLogoIndex = -1;
    std::vector<ResumeDatScanRange> scanRanges;
    std::vector<uint8_t> logoFrameBytes;
    tstring logoPath;
};

struct ResumeDatInfo {
    int64_t srcFileSize = 0;
    int64_t srcWriteTime = 0;
    int requestedServiceId = -1;
    bool captionsParsed = false;
    bool tsreplace = false;
    bool chapter = false;
    bool pmtCut = false;
    double pmtCutSideRate[2] = {};
    std::vector<ResumeDatLogoEntry> logoTable;
    std::vector<uint32_t> detectionLogoIndices;
    std::vector<uint32_t> eraseLogoIndices;
    bool ignoreNoLogo = false;
    bool noLogoInCM = false;
    bool noDelogo = false;
    bool looseLogoDetection = false;
    int autoLogoDetect = 0;
    int autoLogoDetectSearchFrames = 0;
    int autoLogoDetectDivX = 0;
    int autoLogoDetectDivY = 0;
    int autoLogoDetectBlockSize = 0;
    int autoLogoDetectThreshold = 0;
    int autoLogoDetectMarginX = 0;
    int autoLogoDetectMarginY = 0;
    int serviceId = -1;
    int64_t numTotalPackets = 0;
    int64_t numScramblePackets = 0;
    int64_t totalIntVideoSize = 0;
    int64_t splitterSrcFileSize = 0;
    int noDrcsMapCount = 0;
    std::vector<ResumeDatVideoInfo> videos;
};

static bool readUtf8CodePoint(const std::string& value, size_t& offset, uint32_t& codePoint) {
    if (offset >= value.size()) return false;
    const auto first = static_cast<uint8_t>(value[offset++]);
    if (first < 0x80) {
        codePoint = first;
    } else if (first >= 0xC2 && first <= 0xDF) {
        if (offset >= value.size()) return false;
        const auto second = static_cast<uint8_t>(value[offset++]);
        if ((second & 0xC0) != 0x80) return false;
        codePoint = ((first & 0x1F) << 6) | (second & 0x3F);
    } else if (first >= 0xE0 && first <= 0xEF) {
        if (value.size() - offset < 2) return false;
        const auto second = static_cast<uint8_t>(value[offset++]);
        const auto third = static_cast<uint8_t>(value[offset++]);
        if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80
            || (first == 0xE0 && second < 0xA0) || (first == 0xED && second >= 0xA0)) return false;
        codePoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
    } else if (first >= 0xF0 && first <= 0xF4) {
        if (value.size() - offset < 3) return false;
        const auto second = static_cast<uint8_t>(value[offset++]);
        const auto third = static_cast<uint8_t>(value[offset++]);
        const auto fourth = static_cast<uint8_t>(value[offset++]);
        if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80 || (fourth & 0xC0) != 0x80
            || (first == 0xF0 && second < 0x90) || (first == 0xF4 && second >= 0x90)) return false;
        codePoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12)
            | ((third & 0x3F) << 6) | (fourth & 0x3F);
    } else {
        return false;
    }
    return codePoint != 0 && codePoint <= 0x10FFFF
        && !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
}

static void appendUtf8CodePoint(std::string& output, const uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

static tstring resumeDatUtf8ToTString(const std::string& utf8) {
    tstring output;
    size_t offset = 0;
    while (offset < utf8.size()) {
        uint32_t codePoint = 0;
        if (!readUtf8CodePoint(utf8, offset, codePoint)) {
            THROW(FormatException, "Resume.datのUTF-8文字列が不正です");
        }
#ifdef _MSC_VER
        if (codePoint <= 0xFFFF) {
            output.push_back(static_cast<tchar>(codePoint));
        } else {
            const uint32_t value = codePoint - 0x10000;
            output.push_back(static_cast<tchar>(0xD800 + (value >> 10)));
            output.push_back(static_cast<tchar>(0xDC00 + (value & 0x3FF)));
        }
#else
        appendUtf8CodePoint(output, codePoint);
#endif
    }
    return output;
}

static std::string resumeDatTStringToUtf8(const tstring& value) {
#ifdef _MSC_VER
    std::string output;
    for (size_t index = 0; index < value.size(); index++) {
        const uint32_t first = static_cast<uint16_t>(value[index]);
        if (first == 0) {
            THROW(FormatException, "Resume.datの文字列にNULが含まれています");
        }
        uint32_t codePoint = first;
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (++index >= value.size()) {
                THROW(FormatException, "Resume.datのUTF-16文字列が不正です");
            }
            const uint32_t second = static_cast<uint16_t>(value[index]);
            if (second < 0xDC00 || second > 0xDFFF) {
                THROW(FormatException, "Resume.datのUTF-16文字列が不正です");
            }
            codePoint = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
            THROW(FormatException, "Resume.datのUTF-16文字列が不正です");
        }
        appendUtf8CodePoint(output, codePoint);
    }
    return output;
#else
    const std::string output(value.begin(), value.end());
    (void)resumeDatUtf8ToTString(output); // Reject malformed UTF-8 and embedded NUL.
    return output;
#endif
}

static bool isSafeResumeDatLogoFilename(const tstring& filename) {
    return !filename.empty() && filename != _T(".") && filename != _T("..")
        && filename.find(static_cast<tchar>(0)) == tstring::npos
        && filename.find(_T('/')) == tstring::npos
        && filename.find(static_cast<tchar>(92)) == tstring::npos
        && filename.find(_T(':')) == tstring::npos;
}

struct ResumeVideoInfo {
    int numFrames;
    tstring logoPath;
    std::vector<int> trims;
    std::vector<int> divs;
};

struct ResumeInfo {
    int64_t srcFileSize;
    int64_t srcWriteTime;
    int requestedServiceId;
    bool captionsParsed;
    bool tsreplace;
    bool chapter;
    bool pmtCut;
    double pmtCutSideRate[2];
    std::vector<tstring> logoPath;
    std::vector<tstring> eraseLogoPath;
    bool ignoreNoLogo;
    bool noLogoInCM;
    bool noDelogo;
    bool looseLogoDetection;
    int autoLogoDetect;
    int autoLogoDetectSearchFrames;
    int autoLogoDetectDivX;
    int autoLogoDetectDivY;
    int autoLogoDetectBlockSize;
    int autoLogoDetectThreshold;
    int autoLogoDetectMarginX;
    int autoLogoDetectMarginY;
    int serviceId;
    int64_t numTotalPackets;
    int64_t numScramblePackets;
    int64_t totalIntVideoSize;
    int64_t splitterSrcFileSize;
    int noDrcsMapCount;
    std::vector<ResumeVideoInfo> videos;
};

static void writeTString(const File& file, const tstring& str) {
    file.writeArray(std::vector<tchar>(str.begin(), str.end()));
}

static void writeTStringArray(const File& file, const std::vector<tstring>& strings) {
    file.writeValue((int64_t)strings.size());
    for (const auto& str : strings) {
        writeTString(file, str);
    }
}

// タイムコードファイルの最大サイズ (1行10バイト強として約150万フレーム分)
static const int64_t MAX_TIMECODE_FILE_SIZE = 16 * 1024 * 1024;

// タイムコードのコメント行からチャンク境界の出力フレーム番号を読み取る
static void parseChunkBoundaryComment(const std::string& comment, std::vector<int>& boundaries) {
    const size_t tagPos = comment.find(ENCODER_FILTER_CHUNK_BOUNDARY_TAG);
    if (tagPos == std::string::npos) {
        return;
    }
    size_t pos = tagPos + strlen(ENCODER_FILTER_CHUNK_BOUNDARY_TAG);
    while (pos < comment.size()) {
        const size_t first = comment.find_first_of("0123456789", pos);
        if (first == std::string::npos) break;
        const size_t last = comment.find_first_not_of("0123456789", first);
        boundaries.push_back(std::atoi(comment.substr(first, last - first).c_str()));
        if (last == std::string::npos) break;
        pos = last;
    }
}

static std::vector<double> readEncoderFilterTimecodeFile(const tstring& path, std::vector<int>* boundaries = nullptr) {
    std::string text;
    {
        File input(path, _T("rb"));
        const int64_t fileSize = input.size();
        if (fileSize <= 0 || fileSize > MAX_TIMECODE_FILE_SIZE) {
            THROW(FormatException, "エンコーダフィルタのタイムコードが空または大きすぎます");
        }
        text.resize((size_t)fileSize);
        if (input.read(MemoryChunk((uint8_t*)text.data(), (size_t)fileSize)) != (size_t)fileSize) {
            THROW(RuntimeException, "エンコーダフィルタのタイムコードを読み込めません");
        }
    }

    std::vector<double> timestamps;
    size_t begin = 0;
    int lineNumber = 0;
    while (begin < text.size()) {
        const size_t end = text.find('\n', begin);
        std::string line = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        begin = (end == std::string::npos) ? text.size() : end + 1;
        lineNumber++;
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            if (first != std::string::npos && boundaries != nullptr) {
                parseChunkBoundaryComment(line, *boundaries);
            }
            continue;
        }
        try {
            size_t parsed = 0;
            const double timestamp = std::stod(line.substr(first), &parsed);
            if (!std::isfinite(timestamp)
                || line.find_first_not_of(" \t\r", first + parsed) != std::string::npos) {
                THROW(FormatException, "エンコーダフィルタのタイムコード形式が不正です");
            }
            timestamps.push_back(timestamp);
        } catch (const Exception&) {
            throw;
        } catch (...) {
            THROWF(FormatException, "エンコーダフィルタのタイムコード形式が不正です (%d行目)", lineNumber);
        }
    }
    if (timestamps.empty()) {
        THROW(FormatException, "エンコーダフィルタのタイムコードにフレーム情報がありません");
    }
    return timestamps;
}

// エンコーダフィルタ出力タイムコードの実測結果
struct EncoderFilterTimecodeInfo {
    bool isVFR;             // フレーム間隔が一定でない
    int numFrames;          // 実際に出力されたフレーム数
    double frameDurationMs; // CFR時の1フレームの表示時間(ms)
};

// タイムコードの実測値からCFR/VFRとフレームレートを判定する
// フィルタのオプション文字列からフレームレート変化を推測すると、
// 解析漏れがあったときに気づけないまま不整合な出力になるため、常に実測値で判定する
//
// boundaries には分割エンコードのチャンク境界にあたる出力フレーム番号を渡す。
// チャンク境界の時刻は入力フレーム番号から求めるため、フレームレートが変化していると
// 境界のフレーム間隔だけが前後とずれる。この間隔は判定から除外する。
//
// 境界以外のフレーム間隔にわずかでもばらつきがあればVFRとして扱う。
// CFRと誤判定するとタイムコードが反映されず音ズレになるが、逆にVFR扱いにしても
// タイムコードをそのまま反映するだけなので実害がないため、判定は厳しめにしておく。
static EncoderFilterTimecodeInfo analyzeEncoderFilterTimecode(
    const std::vector<double>& timestamps, const std::vector<int>& boundaries = std::vector<int>()) {
    EncoderFilterTimecodeInfo info = {};
    info.numFrames = (int)timestamps.size();
    if (timestamps.size() < 3) {
        // 判定材料が足りないのでCFRとして扱う
        info.frameDurationMs = (timestamps.size() == 2) ? (timestamps[1] - timestamps[0]) : 0.0;
        return info;
    }
    // intervals[i] = timestamps[i + 1] - timestamps[i]
    // チャンク境界のフレーム番号 b に対応する間隔は intervals[b - 1]
    std::vector<bool> ignored(timestamps.size() - 1, false);
    for (const auto boundary : boundaries) {
        if (boundary >= 1 && (size_t)boundary <= ignored.size()) {
            ignored[boundary - 1] = true;
        }
    }
    std::vector<double> intervals;
    intervals.reserve(timestamps.size() - 1);
    for (size_t i = 1; i < timestamps.size(); i++) {
        if (!ignored[i - 1]) {
            intervals.push_back(timestamps[i] - timestamps[i - 1]);
        }
    }
    if (intervals.empty()) {
        return info;
    }
    // 平均ではなく中央値を基準にする
    std::vector<double> sorted = intervals;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double median = sorted[sorted.size() / 2];
    if (!(median > 0.0)) {
        info.isVFR = true;
        return info;
    }
    // タイムベースの丸めによる微小なゆらぎのみCFRとみなす
    // (30fpsと24fpsの差は8.3ms、120fps基準の丸め誤差でも1ms未満なので十分に区別できる)
    const double tolerance = std::max(median * 0.01, 0.05);
    for (const auto interval : intervals) {
        if (std::abs(interval - median) > tolerance) {
            info.isVFR = true;
            return info;
        }
    }
    info.frameDurationMs = median;
    return info;
}

// 実測したフレームレート比を分母8以下の単純な分数で近似する
// (4/5=24fps化、2/1=60fps化、1/2=間引き等を想定)
static bool approximateFpsRatio(double ratio, int& outMul, int& outDiv) {
    if (!(ratio > 0.0)) {
        return false;
    }
    double bestErr = std::numeric_limits<double>::max();
    int bestMul = 0, bestDiv = 0;
    for (int div = 1; div <= 8; div++) {
        const int mul = (int)std::lround(ratio * div);
        if (mul <= 0) continue;
        const double err = std::abs((double)mul / div - ratio);
        if (err < bestErr) {
            bestErr = err;
            bestMul = mul;
            bestDiv = div;
        }
    }
    // 0.5%以上ずれる場合は単純な分数では表せない
    if (bestMul == 0 || bestErr > ratio * 0.005) {
        return false;
    }
    outMul = bestMul;
    outDiv = bestDiv;
    return true;
}

static void writeNormalizedEncoderFilterTimecode(const tstring& path, const std::vector<double>& timestamps) {
    File output(path, _T("wb"));
    const char header[] = "# timecode format v2\n";
    output.write(MemoryChunk((uint8_t*)header, sizeof(header) - 1));
    // 既存のタイムコード出力 (Encoder.cpp createChunkTimecodeFile) と同様、
    // 先頭フレームが0になるように基準を合わせてから整数msに丸める
    const double base = timestamps.front();
    for (const auto timestamp : timestamps) {
        const std::string line = std::to_string((long long)std::llround(timestamp - base)) + "\n";
        output.write(MemoryChunk((uint8_t*)line.data(), line.size()));
    }
}

static tstring readTString(const File& file) {
    const auto chars = file.readArray<tchar>();
    return tstring(chars.begin(), chars.end());
}

static std::vector<tstring> readTStringArray(const File& file) {
    const auto count = file.readValue<int64_t>();
    if (count < 0 || count > INT_MAX) {
        THROW(FormatException, "再開情報の文字列配列数が不正です");
    }
    std::vector<tstring> strings;
    strings.reserve((size_t)count);
    for (int64_t i = 0; i < count; i++) {
        strings.push_back(readTString(file));
    }
    return strings;
}

static int64_t getFileWriteTime(const tstring& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(std::filesystem::path(path), error);
    if (error) {
        THROWF(IOException, "入力ファイルの更新時刻を取得できません: %s", path.c_str());
    }
    return static_cast<int64_t>(time.time_since_epoch().count());
}

static int64_t getFileSize(const tstring& path) {
    File file(path, _T("rb"));
    return file.size();
}

static void saveResumeManifest(
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo,
    const std::vector<std::unique_ptr<CMAnalyze>>& cmanalyze,
    const int serviceId,
    const int64_t numTotalPackets,
    const int64_t numScramblePackets,
    const int64_t totalIntVideoSize,
    const int64_t srcFileSize,
    const int noDrcsMapCount,
    const bool captionsParsed) {
    const auto srcPath = setting.getSrcFilePath();
    const auto pmtCutSideRate = setting.getPmtCutSideRate();
    File file(setting.getTmpResumePath(), _T("wb"));

    file.writeValue(RESUME_MANIFEST_VERSION);
    file.writeValue(srcFileSize);
    file.writeValue(getFileWriteTime(srcPath));

    file.writeValue(setting.getServiceId());
    file.writeValue(captionsParsed);
    file.writeValue(setting.getFormat() == FORMAT_TSREPLACE);
    file.writeValue(setting.isChapterEnabled());
    file.writeValue(setting.isPmtCutEnabled());
    file.writeValue(pmtCutSideRate[0]);
    file.writeValue(pmtCutSideRate[1]);
    writeTStringArray(file, setting.getLogoPath());
    writeTStringArray(file, setting.getEraseLogoPath());
    file.writeValue(setting.isIgnoreNoLogo());
    file.writeValue(setting.isNoLogoInCM());
    file.writeValue(setting.isNoDelogo());
    file.writeValue(setting.isLooseLogoDetection());
    file.writeValue(setting.getAutoLogoDetect());
    file.writeValue(setting.getAutoLogoDetectSearchFrames());
    file.writeValue(setting.getAutoLogoDetectDivX());
    file.writeValue(setting.getAutoLogoDetectDivY());
    file.writeValue(setting.getAutoLogoDetectBlockSize());
    file.writeValue(setting.getAutoLogoDetectThreshold());
    file.writeValue(setting.getAutoLogoDetectMarginX());
    file.writeValue(setting.getAutoLogoDetectMarginY());

    file.writeValue(serviceId);
    file.writeValue(numTotalPackets);
    file.writeValue(numScramblePackets);
    file.writeValue(totalIntVideoSize);
    file.writeValue(srcFileSize);
    file.writeValue(noDrcsMapCount);

    file.writeValue((int)cmanalyze.size());
    for (int videoFileIndex = 0; videoFileIndex < (int)cmanalyze.size(); videoFileIndex++) {
        const auto& cma = cmanalyze[videoFileIndex];
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        file.writeValue(numFrames);
        writeTString(file, cma->getLogoPath());
        file.writeArray(cma->getTrims());
        file.writeArray(cma->getDivs());
    }
}

static ResumeInfo readResumeManifest(const tstring& path) {
    File file(path, _T("rb"));
    const auto version = file.readValue<int>();
    if (version != RESUME_MANIFEST_VERSION) {
        THROWF(FormatException, "再開情報のバージョンが未対応です: %d", version);
    }

    ResumeInfo info;
    info.srcFileSize = file.readValue<int64_t>();
    info.srcWriteTime = file.readValue<int64_t>();
    info.requestedServiceId = file.readValue<int>();
    info.captionsParsed = file.readValue<bool>();
    info.tsreplace = file.readValue<bool>();
    info.chapter = file.readValue<bool>();
    info.pmtCut = file.readValue<bool>();
    info.pmtCutSideRate[0] = file.readValue<double>();
    info.pmtCutSideRate[1] = file.readValue<double>();
    info.logoPath = readTStringArray(file);
    info.eraseLogoPath = readTStringArray(file);
    info.ignoreNoLogo = file.readValue<bool>();
    info.noLogoInCM = file.readValue<bool>();
    info.noDelogo = file.readValue<bool>();
    info.looseLogoDetection = file.readValue<bool>();
    info.autoLogoDetect = file.readValue<int>();
    info.autoLogoDetectSearchFrames = file.readValue<int>();
    info.autoLogoDetectDivX = file.readValue<int>();
    info.autoLogoDetectDivY = file.readValue<int>();
    info.autoLogoDetectBlockSize = file.readValue<int>();
    info.autoLogoDetectThreshold = file.readValue<int>();
    info.autoLogoDetectMarginX = file.readValue<int>();
    info.autoLogoDetectMarginY = file.readValue<int>();
    info.serviceId = file.readValue<int>();
    info.numTotalPackets = file.readValue<int64_t>();
    info.numScramblePackets = file.readValue<int64_t>();
    info.totalIntVideoSize = file.readValue<int64_t>();
    info.splitterSrcFileSize = file.readValue<int64_t>();
    info.noDrcsMapCount = file.readValue<int>();

    const auto videoCount = file.readValue<int>();
    if (videoCount < 0 || videoCount > INT_MAX) {
        THROW(FormatException, "再開情報の映像数が不正です");
    }
    info.videos.resize(videoCount);
    for (auto& video : info.videos) {
        video.numFrames = file.readValue<int>();
        video.logoPath = readTString(file);
        video.trims = file.readArray<int>();
        video.divs = file.readArray<int>();
    }
    return info;
}

static bool convertUnixTimestampToFileTime(
    const int64_t unixSeconds,
    const int64_t nanoseconds,
    int64_t& fileTimeTicks) {
    constexpr int64_t TICKS_PER_SECOND = 10000000LL;
    constexpr int64_t UNIX_TO_FILETIME_TICKS = 116444736000000000LL;
    if (nanoseconds < 0 || nanoseconds >= 1000000000LL) return false;
    const int64_t maxValue = (std::numeric_limits<int64_t>::max)();
    const int64_t minValue = (std::numeric_limits<int64_t>::min)();
    if (unixSeconds > maxValue / TICKS_PER_SECOND
        || unixSeconds < minValue / TICKS_PER_SECOND) {
        return false;
    }
    int64_t ticks = unixSeconds * TICKS_PER_SECOND;
    const int64_t fractionalTicks = nanoseconds / 100;
    if (ticks > maxValue - fractionalTicks) return false;
    ticks += fractionalTicks;
    if (ticks < -UNIX_TO_FILETIME_TICKS
        || ticks > maxValue - UNIX_TO_FILETIME_TICKS) return false;
    fileTimeTicks = ticks + UNIX_TO_FILETIME_TICKS;
    return true;
}

static int64_t getResumeDatFileTime(const tstring& path) {
#ifdef _WIN32
    HANDLE fileHandle = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        THROWF(IOException, "入力ファイルの更新時刻を取得できません: %s", path.c_str());
    }
    struct FileHandleGuard {
        HANDLE handle;
        ~FileHandleGuard() { CloseHandle(handle); }
    } fileHandleGuard{ fileHandle };

    FILETIME lastWriteTime = {};
    if (!GetFileTime(fileHandle, nullptr, nullptr, &lastWriteTime)) {
        THROWF(IOException, "入力ファイルの更新時刻を取得できません: %s", path.c_str());
    }
    const uint64_t ticks = (static_cast<uint64_t>(lastWriteTime.dwHighDateTime) << 32)
        | lastWriteTime.dwLowDateTime;
    if (ticks > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
        THROW(FormatException, "入力ファイルの更新時刻がFILETIMEの範囲外です");
    }
    return static_cast<int64_t>(ticks);
#else
    struct stat attributes = {};
    if (stat(std::filesystem::path(path).c_str(), &attributes) != 0) {
        THROWF(IOException, "入力ファイルの更新時刻を取得できません: %s", path.c_str());
    }
    int64_t fileTimeTicks = 0;
    if (!convertUnixTimestampToFileTime(
        static_cast<int64_t>(attributes.st_mtim.tv_sec),
        static_cast<int64_t>(attributes.st_mtim.tv_nsec),
        fileTimeTicks)) {
        THROW(FormatException, "入力ファイルの更新時刻をFILETIMEへ変換できません");
    }
    return fileTimeTicks;
#endif
}
template<typename T>
static void writeResumeDatValue(const File& file, const T value) {
    static_assert(std::is_integral<T>::value || std::is_floating_point<T>::value,
        "Resume.dat fields must be numeric");
    static_assert(sizeof(T) <= sizeof(uint64_t), "Resume.dat numeric field is too wide");
    uint64_t bits = 0;
    if constexpr (std::is_floating_point<T>::value) {
        static_assert(std::is_same<T, double>::value && sizeof(double) == sizeof(uint64_t)
            && std::numeric_limits<double>::is_iec559, "Resume.dat requires IEEE754 binary64");
        if (!std::isfinite(value)) {
            THROW(FormatException, "Resume.datの浮動小数点値が有限値ではありません");
        }
        std::memcpy(&bits, &value, sizeof(value));
    } else {
        using Unsigned = typename std::make_unsigned<T>::type;
        bits = static_cast<uint64_t>(static_cast<Unsigned>(value));
    }
    std::array<uint8_t, sizeof(T)> bytes = {};
    for (size_t index = 0; index < bytes.size(); index++) {
        bytes[index] = static_cast<uint8_t>((bits >> (index * 8)) & 0xFF);
    }
    file.write(MemoryChunk(bytes.data(), bytes.size()));
}

class ResumeDatReader {
public:
    ResumeDatReader(const File& file, const int64_t limit) : file_(file), limit_(limit) {
        if (limit_ < file_.pos() || limit_ > file_.size()) {
            THROW(FormatException, "Resume.datの読み取り範囲が不正です");
        }
    }

    size_t read(MemoryChunk chunk) const {
        const int64_t remaining = limit_ - file_.pos();
        if (remaining < 0 || static_cast<uint64_t>(chunk.length) > static_cast<uint64_t>(remaining)) {
            THROW(FormatException, "Resume.datの読み取り範囲を超えました");
        }
        const size_t bytesRead = file_.read(chunk);
        if (bytesRead != chunk.length) {
            THROW(FormatException, "Resume.datが途中で終わっています");
        }
        return bytesRead;
    }

    template<typename T>
    T readValue() const {
        static_assert(std::is_integral<T>::value || std::is_floating_point<T>::value,
            "Resume.dat fields must be numeric");
        static_assert(sizeof(T) <= sizeof(uint64_t), "Resume.dat numeric field is too wide");
        std::array<uint8_t, sizeof(T)> bytes = {};
        read(MemoryChunk(bytes.data(), bytes.size()));
        uint64_t bits = 0;
        for (size_t index = 0; index < bytes.size(); index++) {
            bits |= static_cast<uint64_t>(bytes[index]) << (index * 8);
        }
        if constexpr (std::is_floating_point<T>::value) {
            static_assert(std::is_same<T, double>::value && sizeof(double) == sizeof(uint64_t)
                && std::numeric_limits<double>::is_iec559, "Resume.dat requires IEEE754 binary64");
            T value = 0;
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value)) {
                THROW(FormatException, "Resume.datの浮動小数点値が有限値ではありません");
            }
            return value;
        } else {
            using Unsigned = typename std::make_unsigned<T>::type;
            const Unsigned raw = static_cast<Unsigned>(bits);
            if constexpr (std::is_signed<T>::value) {
                const Unsigned signMask = static_cast<Unsigned>(Unsigned(1) << (sizeof(T) * 8 - 1));
                if ((raw & signMask) != 0) {
                    const Unsigned inverted = static_cast<Unsigned>(~raw);
                    return static_cast<T>(-1 - static_cast<T>(inverted));
                }
            }
            return static_cast<T>(raw);
        }
    }

    int64_t pos() const { return file_.pos(); }
    int64_t size() const { return limit_; }

private:
    const File& file_;
    int64_t limit_;
};

static int64_t getResumeDatRemainingBytes(const ResumeDatReader& reader) {
    const int64_t remaining = reader.size() - reader.pos();
    if (remaining < 0) {
        THROW(FormatException, "Resume.datの読み取り位置が不正です");
    }
    return remaining;
}

static bool readResumeDatBool(const ResumeDatReader& reader) {
    const uint8_t value = reader.readValue<uint8_t>();
    if (value > 1) {
        THROW(FormatException, "Resume.datの真偽値が不正です");
    }
    return value != 0;
}

static tstring readResumeDatTString(const ResumeDatReader& reader) {
    const uint32_t byteCount = reader.readValue<uint32_t>();
    const int64_t remaining = getResumeDatRemainingBytes(reader);
    if (byteCount > MAX_RESUME_DAT_STRING_SIZE || static_cast<uint64_t>(byteCount) > static_cast<uint64_t>(remaining)) {
        THROW(FormatException, "Resume.datのUTF-8文字列長が不正です");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(byteCount));
    if (!bytes.empty()) reader.read(MemoryChunk(bytes.data(), bytes.size()));
    return resumeDatUtf8ToTString(std::string(bytes.begin(), bytes.end()));
}

static void writeResumeDatTString(const File& file, const tstring& value) {
    const std::string utf8 = resumeDatTStringToUtf8(value);
    if (utf8.size() > static_cast<size_t>(MAX_RESUME_DAT_STRING_SIZE)) {
        THROW(FormatException, "Resume.datのUTF-8文字列長が上限を超えています");
    }
    writeResumeDatValue(file, static_cast<uint32_t>(utf8.size()));
    if (!utf8.empty()) {
        file.write(MemoryChunk(reinterpret_cast<uint8_t*>(const_cast<char*>(utf8.data())), utf8.size()));
    }
}

static bool resumeDatPathEquals(const tstring& left, const tstring& right, const bool caseInsensitive);

static std::vector<ResumeDatLogoEntry> readResumeDatLogoTable(const ResumeDatReader& reader) {
    const uint32_t count = reader.readValue<uint32_t>();
    const int64_t remaining = getResumeDatRemainingBytes(reader);
    constexpr int64_t MIN_LOGO_TABLE_ENTRY_SIZE = sizeof(uint32_t) + 1 + 32;
    if (count > MAX_RESUME_DAT_LOGO_TABLE_COUNT
        || count > static_cast<uint64_t>(remaining / MIN_LOGO_TABLE_ENTRY_SIZE)) {
        THROW(FormatException, "Resume.datのロゴテーブル数が不正です");
    }
    std::vector<ResumeDatLogoEntry> entries;
    entries.reserve(static_cast<size_t>(count));
    for (uint32_t index = 0; index < count; index++) {
        ResumeDatLogoEntry entry;
        entry.filename = readResumeDatTString(reader);
        if (!isSafeResumeDatLogoFilename(entry.filename)) {
            THROW(FormatException, "Resume.datのロゴbasenameが不正です");
        }
        for (const auto& previous : entries) {
            if (resumeDatPathEquals(entry.filename, previous.filename, true)) {
                THROW(FormatException, "Resume.datのロゴbasenameが重複しています");
            }
        }
        reader.read(MemoryChunk(entry.sha256.data(), entry.sha256.size()));
        entries.push_back(std::move(entry));
    }
    return entries;
}

static void writeResumeDatLogoTable(const File& file, const std::vector<ResumeDatLogoEntry>& entries) {
    if (entries.size() > static_cast<size_t>(MAX_RESUME_DAT_LOGO_TABLE_COUNT)) {
        THROW(FormatException, "Resume.datのロゴテーブル数が上限を超えています");
    }
    writeResumeDatValue(file, static_cast<uint32_t>(entries.size()));
    for (size_t index = 0; index < entries.size(); index++) {
        const auto& entry = entries[index];
        if (!isSafeResumeDatLogoFilename(entry.filename)) {
            THROW(FormatException, "Resume.datに保存するロゴbasenameが不正です");
        }
        for (size_t previous = 0; previous < index; previous++) {
            if (resumeDatPathEquals(entry.filename, entries[previous].filename, true)) {
                THROW(FormatException, "Resume.datに保存するロゴbasenameが重複しています");
            }
        }
        writeResumeDatTString(file, entry.filename);
        file.write(MemoryChunk(const_cast<uint8_t*>(entry.sha256.data()), entry.sha256.size()));
    }
}

static std::vector<uint32_t> readResumeDatLogoIndices(
    const ResumeDatReader& reader, const size_t logoTableSize, const TCHAR* const fieldName) {
    const uint32_t count = reader.readValue<uint32_t>();
    const int64_t remaining = getResumeDatRemainingBytes(reader);
    if (count > MAX_RESUME_DAT_LOGO_COUNT
        || count > static_cast<uint64_t>(remaining / static_cast<int64_t>(sizeof(uint32_t)))) {
        THROWF(FormatException, "Resume.datの%sロゴ数が不正です", fieldName);
    }
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(count));
    for (uint32_t offset = 0; offset < count; offset++) {
        const uint32_t index = reader.readValue<uint32_t>();
        if (index >= logoTableSize || std::find(indices.begin(), indices.end(), index) != indices.end()) {
            THROWF(FormatException, "Resume.datの%sロゴindexが不正です", fieldName);
        }
        indices.push_back(index);
    }
    return indices;
}

static void writeResumeDatLogoIndices(
    const File& file, const std::vector<uint32_t>& indices, const size_t logoTableSize, const TCHAR* const fieldName) {
    if (indices.size() > static_cast<size_t>(MAX_RESUME_DAT_LOGO_COUNT)) {
        THROWF(FormatException, "Resume.datに保存する%sロゴ数が上限を超えています", fieldName);
    }
    writeResumeDatValue(file, static_cast<uint32_t>(indices.size()));
    for (size_t offset = 0; offset < indices.size(); offset++) {
        const uint32_t index = indices[offset];
        if (index >= logoTableSize
            || std::find(indices.begin(), indices.begin() + offset, index) != indices.begin() + offset) {
            THROWF(FormatException, "Resume.datに保存する%sロゴindexが不正です", fieldName);
        }
        writeResumeDatValue(file, index);
    }
}

static std::vector<ResumeDatScanRange> readResumeDatScanRanges(
    const ResumeDatReader& reader, const int32_t numFrames, const int32_t selectedLogoIndex) {
    const uint32_t count = reader.readValue<uint32_t>();
    const int64_t remaining = getResumeDatRemainingBytes(reader);
    if (count > static_cast<uint32_t>(numFrames)
        || count > static_cast<uint64_t>(remaining / (sizeof(int32_t) * 2))
        || (selectedLogoIndex < 0 && count != 0)) {
        THROW(FormatException, "Resume.datのロゴ走査範囲数が不正です");
    }
    std::vector<ResumeDatScanRange> ranges;
    ranges.reserve(static_cast<size_t>(count));
    for (uint32_t index = 0; index < count; index++) {
        ResumeDatScanRange range;
        range.start = reader.readValue<int32_t>();
        range.endExclusive = reader.readValue<int32_t>();
        if (range.start < 0 || range.start >= range.endExclusive || range.endExclusive > numFrames
            || (!ranges.empty() && range.start < ranges.back().endExclusive)) {
            THROW(FormatException, "Resume.datのロゴ走査範囲が不正または重複しています");
        }
        ranges.push_back(range);
    }
    return ranges;
}

static void writeResumeDatScanRanges(
    const File& file, const std::vector<ResumeDatScanRange>& ranges,
    const int32_t numFrames, const int32_t selectedLogoIndex) {
    if (ranges.size() > static_cast<size_t>(numFrames)
        || (selectedLogoIndex < 0 && !ranges.empty())) {
        THROW(FormatException, "Resume.datに保存するロゴ走査範囲数が不正です");
    }
    writeResumeDatValue(file, static_cast<uint32_t>(ranges.size()));
    for (size_t index = 0; index < ranges.size(); index++) {
        const auto& range = ranges[index];
        if (range.start < 0 || range.start >= range.endExclusive || range.endExclusive > numFrames
            || (index > 0 && range.start < ranges[index - 1].endExclusive)) {
            THROW(FormatException, "Resume.datに保存するロゴ走査範囲が不正または重複しています");
        }
        writeResumeDatValue(file, range.start);
        writeResumeDatValue(file, range.endExclusive);
    }
}

static std::vector<uint8_t> readResumeDatLogoFrameBytes(const ResumeDatReader& reader) {
    const uint64_t byteCount = reader.readValue<uint64_t>();
    const int64_t remaining = getResumeDatRemainingBytes(reader);
    if (byteCount > static_cast<uint64_t>(MAX_RESUME_DAT_PAYLOAD_SIZE)
        || byteCount > static_cast<uint64_t>(remaining)) {
        THROW(FormatException, "Resume.datのLogoFrameサイズが不正です");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(byteCount));
    if (!bytes.empty()) reader.read(MemoryChunk(bytes.data(), bytes.size()));
    return bytes;
}

static void writeResumeDatLogoFrameBytes(const File& file, const std::vector<uint8_t>& bytes) {
    if (bytes.size() > static_cast<size_t>(MAX_RESUME_DAT_PAYLOAD_SIZE)) {
        THROW(FormatException, "Resume.datに保存するLogoFrameサイズが上限を超えています");
    }
    writeResumeDatValue(file, static_cast<uint64_t>(bytes.size()));
    if (!bytes.empty()) {
        file.write(MemoryChunk(const_cast<uint8_t*>(bytes.data()), bytes.size()));
    }
}

static ResumeDatInfo readResumeDatHeader(const ResumeDatReader& reader) {
    const int32_t version = reader.readValue<int32_t>();
    if (version != RESUME_MANIFEST_VERSION) {
        THROWF(FormatException, "Resume.dat形式のバージョンが未対応です: %d", version);
    }

    ResumeDatInfo info;
    info.srcFileSize = reader.readValue<int64_t>();
    info.srcWriteTime = reader.readValue<int64_t>();
    info.requestedServiceId = reader.readValue<int32_t>();
    info.serviceId = reader.readValue<int32_t>();
    info.splitterSrcFileSize = reader.readValue<int64_t>();
    info.numTotalPackets = reader.readValue<int64_t>();
    info.numScramblePackets = reader.readValue<int64_t>();
    info.totalIntVideoSize = reader.readValue<int64_t>();

    info.captionsParsed = readResumeDatBool(reader);
    info.noDrcsMapCount = reader.readValue<int32_t>();
    if (info.noDrcsMapCount < 0) {
        THROW(FormatException, "Resume.datのDRCS未登録数が不正です");
    }
    info.tsreplace = readResumeDatBool(reader);
    info.chapter = readResumeDatBool(reader);
    info.pmtCut = readResumeDatBool(reader);
    info.pmtCutSideRate[0] = reader.readValue<double>();
    info.pmtCutSideRate[1] = reader.readValue<double>();

    info.ignoreNoLogo = readResumeDatBool(reader);
    info.noLogoInCM = readResumeDatBool(reader);
    info.noDelogo = readResumeDatBool(reader);
    info.looseLogoDetection = readResumeDatBool(reader);
    info.autoLogoDetect = reader.readValue<int32_t>();
    info.autoLogoDetectSearchFrames = reader.readValue<int32_t>();
    info.autoLogoDetectDivX = reader.readValue<int32_t>();
    info.autoLogoDetectDivY = reader.readValue<int32_t>();
    info.autoLogoDetectBlockSize = reader.readValue<int32_t>();
    info.autoLogoDetectThreshold = reader.readValue<int32_t>();
    info.autoLogoDetectMarginX = reader.readValue<int32_t>();
    info.autoLogoDetectMarginY = reader.readValue<int32_t>();

    info.logoTable = readResumeDatLogoTable(reader);
    info.detectionLogoIndices = readResumeDatLogoIndices(reader, info.logoTable.size(), _T("検出候補"));
    info.eraseLogoIndices = readResumeDatLogoIndices(reader, info.logoTable.size(), _T("消去候補"));

    const uint32_t videoCount = reader.readValue<uint32_t>();
    const int64_t remaining = getResumeDatRemainingBytes(reader);
    if (videoCount > MAX_RESUME_DAT_VIDEO_COUNT
        || videoCount > static_cast<uint64_t>(remaining / RESUME_DAT_VIDEO_RECORD_MIN_SIZE)) {
        THROW(FormatException, "Resume.datの映像数が不正です");
    }
    info.videos.resize(static_cast<size_t>(videoCount));
    return info;
}

static ResumeDatInfo readResumeDatManifest(const tstring& path) {
    File file(path, _T("rb"));
    const int64_t fileSize = file.size();
    ResumeDatReader reader(file, fileSize);
    ResumeDatInfo info = readResumeDatHeader(reader);
    for (auto& video : info.videos) {
        video.numFrames = reader.readValue<int32_t>();
        if (video.numFrames < 0) {
            THROW(FormatException, "Resume.datのフレーム数が不正です");
        }
        video.selectedLogoIndex = reader.readValue<int32_t>();
        if (video.selectedLogoIndex < -1
            || (video.selectedLogoIndex >= 0 && static_cast<size_t>(video.selectedLogoIndex) >= info.logoTable.size())) {
            THROW(FormatException, "Resume.datの選択ロゴindexが不正です");
        }
        video.scanRanges = readResumeDatScanRanges(reader, video.numFrames, video.selectedLogoIndex);
        video.logoFrameBytes = readResumeDatLogoFrameBytes(reader);
        if ((video.selectedLogoIndex < 0 && !video.logoFrameBytes.empty())
            || (video.selectedLogoIndex >= 0 && video.logoFrameBytes.empty())) {
            THROW(FormatException, "Resume.datの選択ロゴとLogoFrameの組み合わせが不正です");
        }
    }
    if (reader.pos() != fileSize || file.size() != fileSize) {
        THROW(FormatException, "Resume.datに未解釈データがあるか、読み込み中にファイルサイズが変化しました");
    }
    return info;
}

static void writeResumeDatInfo(const File& file, const ResumeDatInfo& info) {
    static_assert(sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559,
        "Resume.dat requires IEEE754 binary64");
    if (info.videos.size() > static_cast<size_t>(MAX_RESUME_DAT_VIDEO_COUNT)
        || info.noDrcsMapCount < 0) {
        THROW(FormatException, "Resume.datの映像数またはDRCS未登録数が不正です");
    }
    writeResumeDatValue(file, static_cast<int32_t>(RESUME_MANIFEST_VERSION));
    writeResumeDatValue(file, info.srcFileSize);
    writeResumeDatValue(file, info.srcWriteTime);
    writeResumeDatValue(file, static_cast<int32_t>(info.requestedServiceId));
    writeResumeDatValue(file, static_cast<int32_t>(info.serviceId));
    writeResumeDatValue(file, info.splitterSrcFileSize);
    writeResumeDatValue(file, info.numTotalPackets);
    writeResumeDatValue(file, info.numScramblePackets);
    writeResumeDatValue(file, info.totalIntVideoSize);

    writeResumeDatValue(file, static_cast<uint8_t>(info.captionsParsed ? 1 : 0));
    writeResumeDatValue(file, static_cast<int32_t>(info.noDrcsMapCount));
    writeResumeDatValue(file, static_cast<uint8_t>(info.tsreplace ? 1 : 0));
    writeResumeDatValue(file, static_cast<uint8_t>(info.chapter ? 1 : 0));
    writeResumeDatValue(file, static_cast<uint8_t>(info.pmtCut ? 1 : 0));
    writeResumeDatValue(file, info.pmtCutSideRate[0]);
    writeResumeDatValue(file, info.pmtCutSideRate[1]);

    writeResumeDatValue(file, static_cast<uint8_t>(info.ignoreNoLogo ? 1 : 0));
    writeResumeDatValue(file, static_cast<uint8_t>(info.noLogoInCM ? 1 : 0));
    writeResumeDatValue(file, static_cast<uint8_t>(info.noDelogo ? 1 : 0));
    writeResumeDatValue(file, static_cast<uint8_t>(info.looseLogoDetection ? 1 : 0));
    writeResumeDatValue(file, static_cast<int32_t>(info.autoLogoDetect));
    writeResumeDatValue(file, static_cast<int32_t>(info.autoLogoDetectSearchFrames));
    writeResumeDatValue(file, static_cast<int32_t>(info.autoLogoDetectDivX));
    writeResumeDatValue(file, static_cast<int32_t>(info.autoLogoDetectDivY));
    writeResumeDatValue(file, static_cast<int32_t>(info.autoLogoDetectBlockSize));
    writeResumeDatValue(file, static_cast<int32_t>(info.autoLogoDetectThreshold));
    writeResumeDatValue(file, static_cast<int32_t>(info.autoLogoDetectMarginX));
    writeResumeDatValue(file, static_cast<int32_t>(info.autoLogoDetectMarginY));

    writeResumeDatLogoTable(file, info.logoTable);
    writeResumeDatLogoIndices(file, info.detectionLogoIndices, info.logoTable.size(), _T("検出候補"));
    writeResumeDatLogoIndices(file, info.eraseLogoIndices, info.logoTable.size(), _T("消去候補"));
    writeResumeDatValue(file, static_cast<uint32_t>(info.videos.size()));
    for (const auto& video : info.videos) {
        if (video.numFrames < 0 || video.selectedLogoIndex < -1
            || (video.selectedLogoIndex >= 0 && static_cast<size_t>(video.selectedLogoIndex) >= info.logoTable.size())) {
            THROW(FormatException, "Resume.datに保存するフレーム数または選択ロゴindexが不正です");
        }
        if ((video.selectedLogoIndex < 0 && !video.logoFrameBytes.empty())
            || (video.selectedLogoIndex >= 0 && video.logoFrameBytes.empty())) {
            THROW(FormatException, "Resume.datに保存する選択ロゴとLogoFrameの組み合わせが不正です");
        }
        writeResumeDatValue(file, static_cast<int32_t>(video.numFrames));
        writeResumeDatValue(file, video.selectedLogoIndex);
        writeResumeDatScanRanges(file, video.scanRanges, video.numFrames, video.selectedLogoIndex);
        writeResumeDatLogoFrameBytes(file, video.logoFrameBytes);
    }
}

static tstring getResumeDatFilename(const tstring& path) {
    if (path.empty()) return tstring();
    const auto filename = std::filesystem::path(path).filename().native();
    if (filename.empty() || filename == _T(".") || filename == _T("..")
        || filename.find(static_cast<tchar>(0)) != tstring::npos
        || filename.find(_T('/')) != tstring::npos
        || filename.find(static_cast<tchar>(92)) != tstring::npos
        || filename.find(_T(':')) != tstring::npos) {
        THROW(FormatException, "Resume.datに保存するロゴbasenameが不正です");
    }
    return filename;
}

static std::vector<uint8_t> loadResumeDatLogoFrame(const tstring& path) {
    if (!File::exists(path)) return {};
    File input(path, _T("rb"));
    const int64_t byteCount = input.size();
    if (byteCount <= 0 || byteCount > MAX_RESUME_DAT_PAYLOAD_SIZE) {
        THROW(FormatException, "Resume.datに保存するLogoFrameサイズが不正です");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(byteCount));
    if (input.read(MemoryChunk(bytes.data(), bytes.size())) != bytes.size()) {
        THROW(IOException, "Resume.datに保存するLogoFrameを読み込めません");
    }
    return bytes;
}

static void writeResumeDatManifest(
    const File& file,
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo,
    const std::vector<std::unique_ptr<CMAnalyze>>& cmanalyze,
    const int serviceId,
    const int64_t numTotalPackets,
    const int64_t numScramblePackets,
    const int64_t totalIntVideoSize,
    const int64_t srcFileSize,
    const int noDrcsMapCount,
    const bool captionsParsed) {
    ResumeDatInfo info;
    const auto pmtCutSideRate = setting.getPmtCutSideRate();
    const auto srcPath = setting.getSrcFileOriginalPath();
    info.srcFileSize = srcFileSize;
    info.srcWriteTime = getResumeDatFileTime(srcPath);
    info.requestedServiceId = setting.getServiceId();
    info.captionsParsed = captionsParsed;
    info.tsreplace = setting.getFormat() == FORMAT_TSREPLACE;
    info.chapter = setting.isChapterEnabled();
    info.pmtCut = setting.isPmtCutEnabled();
    info.pmtCutSideRate[0] = pmtCutSideRate[0];
    info.pmtCutSideRate[1] = pmtCutSideRate[1];
    info.ignoreNoLogo = setting.isIgnoreNoLogo();
    info.noLogoInCM = setting.isNoLogoInCM();
    info.noDelogo = setting.isNoDelogo();
    info.looseLogoDetection = setting.isLooseLogoDetection();
    info.autoLogoDetect = setting.getAutoLogoDetect();
    info.autoLogoDetectSearchFrames = setting.getAutoLogoDetectSearchFrames();
    info.autoLogoDetectDivX = setting.getAutoLogoDetectDivX();
    info.autoLogoDetectDivY = setting.getAutoLogoDetectDivY();
    info.autoLogoDetectBlockSize = setting.getAutoLogoDetectBlockSize();
    info.autoLogoDetectThreshold = setting.getAutoLogoDetectThreshold();
    info.autoLogoDetectMarginX = setting.getAutoLogoDetectMarginX();
    info.autoLogoDetectMarginY = setting.getAutoLogoDetectMarginY();
    info.serviceId = serviceId;
    info.numTotalPackets = numTotalPackets;
    info.numScramblePackets = numScramblePackets;
    info.totalIntVideoSize = totalIntVideoSize;
    info.splitterSrcFileSize = srcFileSize;
    info.noDrcsMapCount = noDrcsMapCount;

    std::vector<std::vector<tstring>> logoPathsByEntry;
    auto addLogoEntry = [&](const tstring& path, const tstring& filename,
        const logo::LogoLoadHash& loadedHash) -> uint32_t {
        if (!loadedHash.available || !isSafeResumeDatLogoFilename(filename)) {
            THROW(FormatException, "Resume.datに保存するロゴbasenameまたはLogoData::Load hashが不正です");
        }
        for (uint32_t index = 0; index < info.logoTable.size(); index++) {
            auto& entry = info.logoTable[index];
            if (!resumeDatPathEquals(entry.filename, filename, true)) continue;
            if (entry.sha256 != loadedHash.sha256) {
                THROW(FormatException, "同じロゴbasenameが複数のLogoData::Load内容を参照しています");
            }
            auto& paths = logoPathsByEntry[index];
            if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
                paths.push_back(path);
            }
            return index;
        }
        if (info.logoTable.size() >= static_cast<size_t>(MAX_RESUME_DAT_LOGO_TABLE_COUNT)) {
            THROW(FormatException, "Resume.datのロゴテーブル数が上限を超えています");
        }
        ResumeDatLogoEntry entry;
        entry.filename = filename;
        entry.sha256 = loadedHash.sha256;
        info.logoTable.push_back(std::move(entry));
        logoPathsByEntry.push_back({ path });
        return static_cast<uint32_t>(info.logoTable.size() - 1);
    };

    const auto& detectionPaths = setting.getLogoPath();
    const auto& erasePaths = setting.getEraseLogoPath();
    const size_t expectedLoadedHashCount = detectionPaths.size() + erasePaths.size();
    if (cmanalyze.size() != static_cast<size_t>(reformInfo.getNumVideoFile())) {
        THROW(FormatException, "Resume.datの解析済み映像数が一致しません");
    }
    bool haveCandidateHashes = false;
    for (const auto& cma : cmanalyze) {
        if (!cma->didAttemptResumeDatLogoAnalysis()) continue;
        const auto& hashes = cma->getLogoLoadHashes();
        if (hashes.size() != expectedLoadedHashCount) {
            THROW(FormatException, "LogoData::Loadでロゴ候補全件のhashを取得できません");
        }
        for (const auto& hash : hashes) {
            if (!hash.available) {
                THROW(FormatException, "LogoData::LoadのSHA-256を取得できません");
            }
        }
        if (!haveCandidateHashes) {
            for (size_t index = 0; index < detectionPaths.size(); index++) {
                const tstring filename = getResumeDatFilename(detectionPaths[index]);
                const uint32_t tableIndex = addLogoEntry(detectionPaths[index], filename, hashes[index]);
                info.detectionLogoIndices.push_back(tableIndex);
            }
            for (size_t index = 0; index < erasePaths.size(); index++) {
                const size_t hashIndex = detectionPaths.size() + index;
                const tstring filename = getResumeDatFilename(erasePaths[index]);
                const uint32_t tableIndex = addLogoEntry(erasePaths[index], filename, hashes[hashIndex]);
                info.eraseLogoIndices.push_back(tableIndex);
            }
            haveCandidateHashes = true;
            continue;
        }
        for (size_t index = 0; index < detectionPaths.size(); index++) {
            if (hashes[index].sha256 != info.logoTable[info.detectionLogoIndices[index]].sha256) {
                THROW(FormatException, "検出候補ロゴが映像間で変化しました");
            }
        }
        for (size_t index = 0; index < erasePaths.size(); index++) {
            const size_t hashIndex = detectionPaths.size() + index;
            if (hashes[hashIndex].sha256 != info.logoTable[info.eraseLogoIndices[index]].sha256) {
                THROW(FormatException, "消去候補ロゴが映像間で変化しました");
            }
        }
    }
    if (!haveCandidateHashes && expectedLoadedHashCount != 0) {
        THROW(FormatException, "LogoData::Loadでロゴ候補全件のhashを取得できません");
    }

    info.videos.reserve(cmanalyze.size());
    for (int videoFileIndex = 0; videoFileIndex < static_cast<int>(cmanalyze.size()); videoFileIndex++) {
        const auto& cma = cmanalyze[videoFileIndex];
        ResumeDatVideoInfo video;
        const size_t numFrames = reformInfo.getFilterSourceFrames(videoFileIndex).size();
        if (numFrames > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
            THROW(FormatException, "Resume.datに保存する映像フレーム数が上限を超えています");
        }
        video.numFrames = static_cast<int32_t>(numFrames);
        const tstring selectedLogoFilename = getResumeDatFilename(cma->getLogoPath());
        if (!selectedLogoFilename.empty()) {
            const auto& selectedHash = cma->getSelectedLogoLoadHash();
            video.selectedLogoIndex = static_cast<int32_t>(addLogoEntry(
                cma->getLogoPath(), selectedLogoFilename, selectedHash));
            video.logoFrameBytes = loadResumeDatLogoFrame(setting.getTmpLogoFramePath(videoFileIndex));
            if (video.logoFrameBytes.empty()) {
                THROW(FormatException, "Resume.datに保存する選択ロゴのLogoFrameがありません");
            }
            for (const auto& range : cma->getLogoScanRanges()) {
                video.scanRanges.push_back(ResumeDatScanRange{ range.first, range.second });
            }
        }
        info.videos.push_back(std::move(video));
    }

    for (size_t index = 0; index < info.logoTable.size(); index++) {
        if (logoPathsByEntry[index].empty()) {
            THROW(FormatException, "Resume.datのロゴhashに対応するファイルpathがありません");
        }
        for (const auto& logoPath : logoPathsByEntry[index]) {
            logo::LogoHeader header;
            logo::LogoLoadHash currentHash = {};
            const auto loadedLogo = logo::LogoData::Load(logoPath, &header, &currentHash);
            (void)loadedLogo;
            if (!currentHash.available || currentHash.sha256 != info.logoTable[index].sha256) {
                THROW(FormatException, "LogoData::Load後にロゴファイルが変化しました。Resume.datは更新しません");
            }
        }
    }
    writeResumeDatInfo(file, info);
}

static bool validateResumeSetting(const ConfigWrapper& setting, const ResumeInfo& info, tstring& reason) {
    std::vector<tstring> mismatchedFields;
    if (info.srcFileSize != getFileSize(setting.getSrcFilePath())
        || info.srcWriteTime != getFileWriteTime(setting.getSrcFilePath())) {
        mismatchedFields.push_back(_T("入力TS"));
    }
    if (info.requestedServiceId != setting.getServiceId()) {
        mismatchedFields.push_back(_T("サービスID"));
    }
    if (info.tsreplace != (setting.getFormat() == FORMAT_TSREPLACE)) {
        mismatchedFields.push_back(_T("TS置換出力"));
    }
    if (info.eraseLogoPath != setting.getEraseLogoPath()) {
        mismatchedFields.push_back(_T("追加ロゴ消し設定"));
    }
    if (info.noLogoInCM != setting.isNoLogoInCM()) {
        mismatchedFields.push_back(_T("CM解析でロゴを使用しない設定"));
    }
    if (mismatchedFields.empty()) {
        return true;
    }

    reason = _T("再開情報と設定が一致しません(");
    for (int index = 0; index < (int)mismatchedFields.size(); index++) {
        if (index > 0) {
            reason += _T(", ");
        }
        reason += mismatchedFields[index];
    }
    reason += _T(")");
    return false;
}

static bool validateResumeFiles(
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    const ResumeInfo& info,
    tstring& reason) {
    const int numVideoFiles = reformInfo.getNumVideoFile();
    if ((int)info.videos.size() != numVideoFiles) {
        reason = _T("再開情報の映像数が一致しません");
        return false;
    }
    if (!File::exists(setting.getAudioFilePath()) || !File::exists(setting.getWaveFilePath())) {
        reason = _T("再開に必要な音声一時ファイルがありません");
        return false;
    }
    if (isTsReadExAvailable(setting) && !File::exists(setting.getTmpTsReadExDumpPath())) {
        reason = _T("再開に必要なtsreadex_dump.txtがありません");
        return false;
    }

    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; videoFileIndex++) {
        const auto& video = info.videos[videoFileIndex];
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        if (video.numFrames != numFrames) {
            reason = StringFormat(_T("再開情報のフレーム数が一致しません: %d"), videoFileIndex);
            return false;
        }
        if (video.trims.size() % 2 != 0) {
            reason = StringFormat(_T("再開情報のTrim区間数が不正です: %d"), videoFileIndex);
            return false;
        }
        for (int i = 0; i < (int)video.trims.size(); i += 2) {
            if (video.trims[i] < 0 || video.trims[i + 1] < video.trims[i]
                || video.trims[i + 1] > numFrames) {
                reason = StringFormat(_T("再開情報のTrim区間が不正です: %d"), videoFileIndex);
                return false;
            }
        }
        if (video.divs.size() > 0) {
            if (video.divs.size() < 2 || video.divs.front() != 0 || video.divs.back() != numFrames) {
                reason = StringFormat(_T("再開情報の分割情報が不正です: %d"), videoFileIndex);
                return false;
            }
            for (int i = 1; i < (int)video.divs.size(); i++) {
                if (video.divs[i] < video.divs[i - 1]) {
                    reason = StringFormat(_T("再開情報の分割順序が不正です: %d"), videoFileIndex);
                    return false;
                }
            }
        }
        const bool requiresSavedLogo = !setting.isNoDelogo()
            && setting.getLogoPath().size() > 0
            && numFrames >= 300;
        if (requiresSavedLogo && video.logoPath.empty()) {
            reason = StringFormat(_T("ロゴ消しに必要な保存済みロゴ情報がありません: %d"), videoFileIndex);
            return false;
        }
        if (!File::exists(setting.getIntVideoFilePath(videoFileIndex))
            || !File::exists(setting.getTmpAMTSourcePath(videoFileIndex))) {
            reason = StringFormat(_T("再開に必要な映像一時ファイルがありません: %d"), videoFileIndex);
            return false;
        }
        if (video.divs.size() > 0 && !File::exists(setting.getTmpTrimAVSPath(videoFileIndex))) {
            reason = StringFormat(_T("再開に必要なTrimファイルがありません: %d"), videoFileIndex);
            return false;
        }
        if (setting.isChapterEnabled() && numFrames >= 300
            && !File::exists(setting.getTmpJlsPath(videoFileIndex))) {
            reason = StringFormat(_T("再開に必要なチャプター情報がありません: %d"), videoFileIndex);
            return false;
        }
        if (video.logoPath.size() > 0 && !File::exists(video.logoPath)) {
            reason = StringFormat(_T("再開に必要なロゴファイルがありません: %d"), videoFileIndex);
            return false;
        }
    }
    return true;
}

struct ResumeDatLogoFilePath {
    std::filesystem::path parent;
    tstring basename;
};

static bool isResumeDatAsciiDigit(const tchar character) {
    return character >= _T('0') && character <= _T('9');
}

static tchar foldResumeDatPathCharacter(const tchar character) {
    return character >= _T('A') && character <= _T('Z')
        ? static_cast<tchar>(character + (_T('a') - _T('A'))) : character;
}

static bool resumeDatPathEquals(const tstring& left, const tstring& right, const bool caseInsensitive) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); index++) {
        const tchar leftCharacter = caseInsensitive
            ? foldResumeDatPathCharacter(left[index]) : left[index];
        const tchar rightCharacter = caseInsensitive
            ? foldResumeDatPathCharacter(right[index]) : right[index];
        if (leftCharacter != rightCharacter) return false;
    }
    return true;
}

static bool resumeDatLogoNamesCollide(const tstring& left, const tstring& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); index++) {
        const tchar leftCharacter = foldResumeDatPathCharacter(left[index]);
        const tchar rightCharacter = foldResumeDatPathCharacter(right[index]);
        if (leftCharacter != rightCharacter) return false;
    }
    return true;
}

static bool resolveResumeDatLogoFilePath(
    const tstring& path, ResumeDatLogoFilePath& result, tstring& reason) {
    if (path.empty() || path.find(static_cast<tchar>(0)) != tstring::npos) {
        reason = _T("ロゴpathが空、またはNULを含んでいます");
        return false;
    }
    result.basename = PathGetFilename(path);
    if (!isSafeResumeDatLogoFilename(result.basename)) {
        reason = _T("現在のlogo pathのbasenameが不正です");
        return false;
    }

    const std::filesystem::path filePath(path);
    auto lexicalParent = filePath.parent_path();
    if (lexicalParent.empty()) lexicalParent = std::filesystem::path(_T("."));

    std::error_code parentError, fileError, rootStatusError, fileStatusError;
    result.parent = std::filesystem::canonical(lexicalParent, parentError);
    const auto resolvedFile = std::filesystem::canonical(filePath, fileError);
    if (parentError || fileError
        || !std::filesystem::is_directory(result.parent, rootStatusError)
        || rootStatusError
        || !std::filesystem::is_regular_file(resolvedFile, fileStatusError)
        || fileStatusError) {
        reason = _T("現在のlogo rootまたはfileを実体解決できません");
        return false;
    }
    if (!rgy_path_is_same(resolvedFile.parent_path().native(), result.parent.native())) {
        reason = _T("現在のlogo fileのcanonical pathがlogo root直下ではありません");
        return false;
    }
    return true;
}

static bool hasResumeDatAutoLogoName(const tstring& basename, const int serviceId) {
    const tstring prefix = StringFormat(_T("SID%d-auto-"), serviceId);
    constexpr size_t AUTO_TIMESTAMP_LENGTH = 15; // YYYYMMDD_HHMMSS
    constexpr size_t LOGO_EXTENSION_LENGTH = 4;
    if (basename.size() != prefix.size() + AUTO_TIMESTAMP_LENGTH + LOGO_EXTENSION_LENGTH
        || basename.compare(0, prefix.size(), prefix) != 0
        || basename.compare(basename.size() - 4, 4, _T(".lgd")) != 0) {
        return false;
    }
    const size_t timestampStart = prefix.size();
    for (size_t offset = 0; offset < AUTO_TIMESTAMP_LENGTH; offset++) {
        const tchar character = basename[timestampStart + offset];
        if (offset == 8) {
            if (character != _T('_')) return false;
        } else if (!isResumeDatAsciiDigit(character)) {
            return false;
        }
    }
    return true;
}

static bool rebaseResumeDatLogoPaths(
    ResumeDatInfo& info,
    const std::vector<tstring>& currentLogoPaths,
    tstring& reason) {
    reason.clear();
    if (!info.eraseLogoIndices.empty()) {
        reason = _T("Resume.datは追加ロゴ消し用の複数LogoFrameを保存しないため再利用できません");
        return false;
    }
    if (info.detectionLogoIndices.size() != currentLogoPaths.size()) {
        reason = _T("保存元と現在のロゴ設定件数が一致しません");
        return false;
    }
    if (info.detectionLogoIndices.empty()) {
        for (auto& video : info.videos) {
            if (video.selectedLogoIndex >= 0) {
                reason = _T("現在のlogo rootがなく、保存済み動画ロゴを解決できません");
                return false;
            }
            video.logoPath.clear();
        }
        return true;
    }

    std::vector<ResumeDatLogoFilePath> currentPaths;
    currentPaths.reserve(currentLogoPaths.size());
    for (size_t index = 0; index < currentLogoPaths.size(); index++) {
        const uint32_t logoIndex = info.detectionLogoIndices[index];
        if (logoIndex >= info.logoTable.size()) {
            reason = _T("保存されたロゴ候補indexが不正です");
            return false;
        }
        const auto& savedFilename = info.logoTable[logoIndex].filename;
        if (!isSafeResumeDatLogoFilename(savedFilename)) {
            reason = _T("保存されたロゴ設定basenameが不正です");
            return false;
        }
        ResumeDatLogoFilePath currentPath;
        if (!resolveResumeDatLogoFilePath(currentLogoPaths[index], currentPath, reason)) return false;
        if (!resumeDatPathEquals(savedFilename, currentPath.basename, false)) {
            reason = _T("保存元と現在のロゴ設定basenameまたは順序が一致しません");
            return false;
        }
        for (size_t previous = 0; previous < index; previous++) {
            const auto& previousFilename = info.logoTable[info.detectionLogoIndices[previous]].filename;
            if (resumeDatLogoNamesCollide(savedFilename, previousFilename)
                || resumeDatLogoNamesCollide(currentPath.basename, currentPaths[previous].basename)) {
                reason = _T("ロゴbasenameが衝突して対応付けできません");
                return false;
            }
        }
        currentPaths.push_back(std::move(currentPath));
    }

    const auto currentRoot = currentPaths.front().parent;
    for (const auto& currentPath : currentPaths) {
        if (!rgy_path_is_same(currentPath.parent.native(), currentRoot.native())) {
            reason = _T("現在のロゴ設定が複数parentに分かれています");
            return false;
        }
    }

    for (auto& video : info.videos) {
        if (video.selectedLogoIndex < 0) {
            video.logoPath.clear();
            continue;
        }
        if (static_cast<size_t>(video.selectedLogoIndex) >= info.logoTable.size()) {
            reason = _T("保存されたvideo.logo indexが不正です");
            return false;
        }
        const auto& selectedFilename = info.logoTable[video.selectedLogoIndex].filename;
        if (!isSafeResumeDatLogoFilename(selectedFilename)) {
            reason = _T("保存されたvideo.logo basenameが不正です");
            return false;
        }
        bool matchedSettingEntry = false;
        for (const auto& currentPath : currentPaths) {
            if (resumeDatPathEquals(currentPath.basename, selectedFilename, false)) {
                matchedSettingEntry = true;
                break;
            }
        }
        if (!matchedSettingEntry && !hasResumeDatAutoLogoName(selectedFilename, info.serviceId)) {
            reason = _T("保存されたvideo.logoが設定entryまたは自動生成ロゴではありません");
            return false;
        }
        const auto videoLogoPath = currentRoot / std::filesystem::path(selectedFilename);
        ResumeDatLogoFilePath resolvedVideoPath;
        if (!resolveResumeDatLogoFilePath(videoLogoPath.native(), resolvedVideoPath, reason)) return false;
        video.logoPath = videoLogoPath.native();
    }
    return true;
}

static bool validateResumeDatSettings(
    const ConfigWrapper& setting,
    const ResumeDatInfo& info,
    StreamReformInfo& reformInfo,
    const int serviceId,
    const int64_t numTotalPackets,
    const int64_t numScramblePackets,
    const int64_t totalIntVideoSize,
    const int64_t srcFileSize,
    tstring& reason) {
    std::vector<tstring> mismatchedFields;
    auto addMismatch = [&](const tchar* field) {
        mismatchedFields.push_back(field);
    };
    const auto srcPath = setting.getSrcFileOriginalPath();
    if (info.srcFileSize != getFileSize(srcPath)
        || info.srcWriteTime != getResumeDatFileTime(srcPath)) {
        addMismatch(_T("入力TS"));
    }
    if (info.requestedServiceId != setting.getServiceId()) addMismatch(_T("要求サービスID"));
    if (info.tsreplace != (setting.getFormat() == FORMAT_TSREPLACE)) addMismatch(_T("TS置換出力"));
    if (info.looseLogoDetection != setting.isLooseLogoDetection()) addMismatch(_T("ロゴ検出しきい値設定"));
    if (info.serviceId != serviceId) addMismatch(_T("実サービスID"));
    if (info.numTotalPackets != numTotalPackets) addMismatch(_T("入力パケット数"));
    if (info.numScramblePackets != numScramblePackets) addMismatch(_T("スクランブルパケット数"));
    if (info.totalIntVideoSize != totalIntVideoSize) addMismatch(_T("映像サイズ"));
    if (info.splitterSrcFileSize != srcFileSize || info.splitterSrcFileSize != info.srcFileSize) {
        addMismatch(_T("TS解析サイズ"));
    }
    if (!mismatchedFields.empty()) {
        reason = _T("Resume.datと解析条件が一致しません(");
        for (size_t index = 0; index < mismatchedFields.size(); index++) {
            if (index > 0) reason += _T(", ");
            reason += mismatchedFields[index];
        }
        reason += _T(")");
        return false;
    }

    // Resume.dat stores only the selected logo LogoFrame; erase-logo frames remain unsupported for reuse.
    if (!info.eraseLogoIndices.empty()) {
        reason = _T("Resume.datは追加ロゴ消し用の複数LogoFrameを保存しないため再利用できません");
        return false;
    }
    if ((int)info.videos.size() != reformInfo.getNumVideoFile()) {
        reason = _T("Resume.datの映像数が一致しません");
        return false;
    }
    for (int videoFileIndex = 0; videoFileIndex < reformInfo.getNumVideoFile(); videoFileIndex++) {
        const auto& video = info.videos[videoFileIndex];
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        if (video.numFrames != numFrames) {
            reason = StringFormat(_T("Resume.datのフレーム数が一致しません: %d"), videoFileIndex);
            return false;
        }
        if ((video.selectedLogoIndex < 0 && !video.logoPath.empty())
            || (video.selectedLogoIndex >= 0 && video.logoPath.empty())) {
            reason = StringFormat(_T("Resume.datの選択ロゴindexとpathが一致しません: %d"), videoFileIndex);
            return false;
        }
    }
    return true;
}

static bool tryLoadResumeDat(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    ResumeDatInfo& info,
    StreamReformInfo& reformInfo,
    const int serviceId,
    const int64_t numTotalPackets,
    const int64_t numScramblePackets,
    const int64_t totalIntVideoSize,
    const int64_t srcFileSize) {
    const auto path = setting.getResumeDatPath();
    if (path.empty()) {
        return false;
    }
    try {
        if (!File::exists(path)) {
            ctx.infoF(_T("[Resume.dat再利用] ファイルがないため通常処理へ進みます: %s"), path.c_str());
            return false;
        }
        info = readResumeDatManifest(path);
        tstring reason;
        if (!rebaseResumeDatLogoPaths(info, setting.getLogoPath(), reason)) {
            ctx.warnF(_T("[Resume.dat再利用] %s。通常処理へ戻ります"), reason.c_str());
            return false;
        }
        if (!validateResumeDatSettings(setting, info, reformInfo,
            serviceId, numTotalPackets, numScramblePackets, totalIntVideoSize,
            srcFileSize, reason)) {
            ctx.warnF(_T("[Resume.dat再利用] %s。通常処理へ戻ります"), reason.c_str());
            return false;
        }
        ctx.infoF(_T("[Resume.dat再利用] 候補を読み込みました: %s"), path.c_str());
        return true;
    } catch (const Exception& e) {
        ctx.warnF(_T("[Resume.dat再利用] 読み込みに失敗したため通常処理へ戻ります: %s"), e.message());
    } catch (const std::exception& e) {
        ctx.warnF(_T("[Resume.dat再利用] 読み込みに失敗したため通常処理へ戻ります: %s"), char_to_tstring(e.what()));
    }
    return false;
}

static ResumeDatLogoReuseData makeResumeDatLogoReuseData(
    const ResumeDatInfo& info, const size_t videoFileIndex) {
    ResumeDatLogoReuseData reuse;
    reuse.hasCachedCandidateData = true;
    const auto& video = info.videos.at(videoFileIndex);
    reuse.hasSelectedLogo = video.selectedLogoIndex >= 0;
    if (reuse.hasSelectedLogo) {
        const auto& selected = info.logoTable.at(static_cast<size_t>(video.selectedLogoIndex));
        reuse.selectedLogoPath = video.logoPath;
        reuse.selectedLogoHash = selected.sha256;
        reuse.logoFrameBytes = video.logoFrameBytes;
        for (const auto& range : video.scanRanges) {
            reuse.scanRanges.push_back({ range.start, range.endExclusive });
        }
    }
    for (const uint32_t index : info.detectionLogoIndices) {
        reuse.detectionLogoHashes.push_back(info.logoTable.at(index).sha256);
    }
    for (const uint32_t index : info.eraseLogoIndices) {
        reuse.eraseLogoHashes.push_back(info.logoTable.at(index).sha256);
    }
    return reuse;
}

static void saveResumeDatSidecar(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo,
    const std::vector<std::unique_ptr<CMAnalyze>>& cmanalyze,
    const int serviceId,
    const int64_t numTotalPackets,
    const int64_t numScramblePackets,
    const int64_t totalIntVideoSize,
    const int64_t srcFileSize,
    const int noDrcsMapCount,
    const bool captionsParsed) {
    const auto path = setting.getResumeDatPath();
    if (path.empty()) {
        return;
    }
    static std::atomic<uint64_t> stageSequence{ 0 };
    const auto writingPath = StringFormat(_T("%s.%llu.%llu.writing"),
        path.c_str(),
        (unsigned long long)GetCurrentProcessId(),
        (unsigned long long)stageSequence.fetch_add(1));
    try {
        {
            File file(writingPath, _T("wb"));
            writeResumeDatManifest(file, setting, reformInfo, cmanalyze,
                serviceId, numTotalPackets, numScramblePackets, totalIntVideoSize,
                srcFileSize, noDrcsMapCount, captionsParsed);
            file.flush();
        }
        readResumeDatManifest(writingPath);
#ifdef _WIN32
        if (!MoveFileEx(writingPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            THROWF(IOException, "Resume.datを置き換えられませんでした (error=%d)", (int)GetLastError());
        }
#else
        std::filesystem::rename(writingPath, path);
#endif
        ctx.infoF(_T("[Resume.dat再利用] 解析結果を保存しました: %s"), path.c_str());
    } catch (const Exception& e) {
        rgy_file_remove(writingPath.c_str());
        ctx.warnF(_T("[Resume.dat再利用] 保存に失敗しました: %s"), e.message());
    } catch (const std::exception& e) {
        rgy_file_remove(writingPath.c_str());
        ctx.warnF(_T("[Resume.dat再利用] 保存に失敗しました: %s"), char_to_tstring(e.what()));
    }
}

static bool tryLoadResume(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    ResumeInfo& info,
    std::unique_ptr<StreamReformInfo>& reformInfo) {
    if (setting.getResumeDir().size() == 0) {
        return false;
    }
    if (!rgy_path_is_same(setting.getTmpDir(), setting.getResumeDir())) {
        ctx.warnF(_T("[一時ファイル再利用] 指定された再開フォルダを使用できないため通常処理へ戻ります: %s"), setting.getResumeDir().c_str());
        return false;
    }

    try {
        tstring reason;
        ctx.info(_T("[一時ファイル再利用] 再開情報を検証します"));
        if (!File::exists(setting.getTmpStreamInfoPath()) || !File::exists(setting.getTmpResumePath())) {
            ctx.warn(_T("[一時ファイル再利用] 再開情報が見つからないため通常処理へ戻ります"));
            return false;
        }
        info = readResumeManifest(setting.getTmpResumePath());
        ctx.info(_T("[一時ファイル再利用] 再開マニフェストを読み込みました"));
        if (!validateResumeSetting(setting, info, reason)) {
            ctx.warnF(_T("[一時ファイル再利用] %s。通常処理へ戻ります"), reason.c_str());
            return false;
        }
        if (setting.isSubtitlesEnabled() && !info.captionsParsed) {
            ctx.warn(_T("[一時ファイル再利用] 再開情報に字幕解析結果が含まれていないため通常処理へ戻ります"));
            return false;
        }
        reformInfo = std::make_unique<StreamReformInfo>(StreamReformInfo::deserialize(ctx, setting.getTmpStreamInfoPath()));
        if (!setting.isSubtitlesEnabled()) {
            reformInfo->clearCaptionItems();
        }
        reformInfo->prepare(setting.isSplitSub(), setting.isEncodeAudio(), setting.getFormat() == FORMAT_TSREPLACE,
            setting.getAudioFormatChangeMode(), setting.getAudioFilePath());
        ctx.info(_T("[一時ファイル再利用] ストリーム情報を読み込みました"));
        if (!validateResumeFiles(setting, *reformInfo, info, reason)) {
            ctx.warnF(_T("[一時ファイル再利用] %s。通常処理へ戻ります"), reason.c_str());
            reformInfo.reset();
            return false;
        }
        ctx.infoF(_T("[一時ファイル再利用] 再開情報を読み込みました: %s"), setting.getTmpResumePath().c_str());
        return true;
    } catch (const Exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] 再開情報の検証に失敗したため通常処理へ戻ります: %s"), e.message());
    } catch (const std::exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] 再開情報の検証に失敗したため通常処理へ戻ります: %s"), char_to_tstring(e.what()));
    }
    reformInfo.reset();
    return false;
}

static void saveResumeFiles(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo,
    const std::vector<std::unique_ptr<CMAnalyze>>& cmanalyze,
    const int serviceId,
    const int64_t numTotalPackets,
    const int64_t numScramblePackets,
    const int64_t totalIntVideoSize,
    const int64_t srcFileSize,
    const int noDrcsMapCount,
    const bool captionsParsed) {
    try {
        saveResumeManifest(setting, reformInfo, cmanalyze,
            serviceId, numTotalPackets, numScramblePackets, totalIntVideoSize, srcFileSize,
            noDrcsMapCount, captionsParsed);
        ctx.infoF(_T("[一時ファイル再利用] 再開情報を保存しました: %s"), setting.getTmpResumePath().c_str());
    } catch (const Exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] 再開情報の保存に失敗しました: %s"), e.message());
    } catch (const std::exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] 再開情報の保存に失敗しました: %s"), char_to_tstring(e.what()));
    }
}

static void saveResumeStreamInfo(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo) {
    try {
        reformInfo.serialize(setting.getTmpStreamInfoPath());
        ctx.infoF(_T("[一時ファイル再利用] ストリーム情報を保存しました: %s"), setting.getTmpStreamInfoPath().c_str());
    } catch (const Exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] ストリーム情報の保存に失敗しました: %s"), e.message());
    } catch (const std::exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] ストリーム情報の保存に失敗しました: %s"), char_to_tstring(e.what()));
    }
}

static bool isCaptionParsingEnabled(const ConfigWrapper& setting) {
    return setting.isSubtitlesEnabled() || setting.isNoRemoveTmp();
}

static void copyCutInfoForCMOnly(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    const std::vector<std::unique_ptr<CMAnalyze>>& cmanalyze,
    const int numVideoFiles) {
    // 複数映像がある場合、動画時間（フレーム数）が最も長いもののカット情報をコピー
    int bestIndex = -1;
    int bestFrames = -1;
    tstring bestSrcTrim;
    for (int vindex = 0; vindex < numVideoFiles; vindex++) {
        const auto srcTrim = setting.getTmpTrimAVSPath(vindex);
        if (!File::exists(srcTrim)) {
            continue;
        }
        const int numFrames = (int)reformInfo.getFilterSourceFrames(vindex).size();
        if (bestIndex < 0 || numFrames > bestFrames) {
            bestIndex = vindex;
            bestFrames = numFrames;
            bestSrcTrim = srcTrim;
        }
    }
    if (bestIndex >= 0) {
        const auto dstTrim = StringFormat(_T("%s.trim.avs"), setting.getSrcFileOriginalPath().c_str());
        ctx.infoF(_T("[CM解析のみ] trim%d.avs をtrim.avsとしてコピー: %s"),
            bestIndex, dstTrim.c_str());
        if (!rgy_file_copy(bestSrcTrim, dstTrim, true)) {
            ctx.warnF(_T("[CM解析のみ] trim.avsのコピーに失敗: %s -> %s"),
                bestSrcTrim.c_str(), dstTrim.c_str());
        }
    } else {
        ctx.warn(_T("[CM解析のみ] コピー対象のtrim*.avsが見つかりませんでした"));
    }

    if (bestIndex >= 0 && File::exists(setting.getTmpDivPath(bestIndex))) {
        const auto dstDiv = StringFormat(_T("%s.div.txt"), setting.getSrcFileOriginalPath().c_str());
        const int numFrames = (int)reformInfo.getFilterSourceFrames(bestIndex).size();
        StringBuilder sb;
        for (const auto point : cmanalyze[bestIndex]->getDivs()) {
            if (point > 0 && point < numFrames) {
                sb.append("%d\n", point);
            }
        }
        ctx.infoF(_T("[CM解析のみ] div%d.txt をdiv.txtとしてコピー: %s"),
            bestIndex, dstDiv.c_str());
        try {
            File file(dstDiv, _T("w"));
            file.write(sb.getMC());
        } catch (const Exception& e) {
            ctx.warnF(_T("[CM解析のみ] div.txtのコピーに失敗: %s -> %s (%s)"),
                setting.getTmpDivPath(bestIndex).c_str(), dstDiv.c_str(), e.message());
        }
    } else if (bestIndex >= 0) {
        ctx.warnF(_T("[CM解析のみ] コピー対象のdiv%d.txtが見つかりませんでした"), bestIndex);
    }
}

static tstring createWhisperWaveInput(AMTContext& ctx,
                                      const ConfigWrapper& setting,
                                      const StreamReformInfo& reformInfo,
                                      const WhisperAudioEntry& entry) {
    const int bytesPerSample = 2;
    const int srcChannels = 2; // AMTSplitter outputs 16bit stereo PCM for whisper input
    const int destChannels = (entry.dualMonoChannel >= 0) ? 1 : srcChannels;

    if (entry.audioSourceIndex < 0) {
        ctx.warn(_T("Whisper入力wav作成: 音声インデックスが不正です"));
        return tstring();
    }

    const auto& key = entry.key;
    const auto& fileIn = reformInfo.getEncodeFile(key);
    if (entry.audioSourceIndex >= (int)fileIn.audioFrames.size()) {
        ctx.warnF(_T("Whisper入力wav作成: 音声%d-%dで音声インデックス%dが範囲外です"), key.video, key.format, entry.audioSourceIndex);
        return tstring();
    }

    std::vector<int> plannedFrames;
    if (fileIn.isAudioTrackPlanned) {
        const auto& track = fileIn.audioTrackPlan.at(entry.localIndex);
        for (const auto& ref : track.frames) {
            plannedFrames.push_back(ref.operation != AudioTrackOperation::SILENCE ? ref.frameIndex : -1);
        }
    }
    const auto& frameIndexList = !fileIn.isAudioTrackPlanned
        ? fileIn.audioFrames[entry.audioSourceIndex] : plannedFrames;
    if (frameIndexList.empty()) {
        ctx.warnF(_T("Whisper入力wav作成: 音声%d-%d-%dにフレームが存在しません"), key.video, key.format, entry.audioSourceIndex);
        return tstring();
    }

    auto waveFrames = reformInfo.getWaveInput(frameIndexList);
    if (waveFrames.empty()) {
        ctx.warnF(_T("Whisper入力wav作成: 音声%d-%d-%dのwave情報が取得できません"), key.video, key.format, entry.audioSourceIndex);
        return tstring();
    }

    int samplesPerFrame = 0;
    for (const auto& frame : waveFrames) {
        if (frame.waveLength > 0) {
            samplesPerFrame = (int)(frame.waveLength / (bytesPerSample * srcChannels));
            break;
        }
    }
    if (samplesPerFrame == 0) {
        samplesPerFrame = 1024;
    }

    int64_t totalSamples = 0;
    for (const auto& frame : waveFrames) {
        int frameSamples = (frame.waveLength > 0)
            ? (int)(frame.waveLength / (bytesPerSample * srcChannels))
            : samplesPerFrame;
        totalSamples += frameSamples;
    }

    const auto fmt = reformInfo.getFormat(key);
    int sampleRate = 48000;
    if (fileIn.isAudioTrackPlanned) {
        sampleRate = fileIn.audioTrackPlan.at(entry.localIndex).sampleRate;
    } else if (entry.audioSourceIndex < (int)fmt.audioFormat.size() && fmt.audioFormat[entry.audioSourceIndex].sampleRate > 0) {
        sampleRate = fmt.audioFormat[entry.audioSourceIndex].sampleRate;
    }

    const tstring wavPath = setting.getTmpWhisperWavPath(key, entry.localIndex);
    auto closeFile = [](FILE* file) { fclose(file); };
    std::unique_ptr<FILE, decltype(closeFile)> fp(_tfopen(wavPath.c_str(), _T("wb")), closeFile);
    if (!fp) {
        ctx.warnF(_T("Whisper入力wav作成: ファイルを開けません (%s)"), wavPath.c_str());
        return tstring();
    }

    writeWaveHeader(fp.get(), destChannels, sampleRate, bytesPerSample * 8, totalSamples);

    File sourceWave(setting.getWaveFilePath(), _T("rb"));
    std::vector<uint8_t> srcBuffer;
    std::vector<uint8_t> dstBuffer;

    for (size_t waveIndex = 0; waveIndex < waveFrames.size(); waveIndex++) {
        const auto& frame = waveFrames[waveIndex];
        const int sourceDualMonoChannel = fileIn.isAudioTrackPlanned
            ? fileIn.audioTrackPlan.at(entry.localIndex).frames[waveIndex].dualMonoChannel : entry.dualMonoChannel;
        int frameSamples = (frame.waveLength > 0)
            ? (int)(frame.waveLength / (bytesPerSample * srcChannels))
            : samplesPerFrame;
        if (frameSamples <= 0) {
            continue;
        }

        size_t srcBytes = (size_t)frameSamples * bytesPerSample * srcChannels;
        srcBuffer.assign(srcBytes, 0);
        if (frame.waveLength > 0) {
            sourceWave.seek(frame.waveOffset, SEEK_SET);
            MemoryChunk chunk(srcBuffer.data(), frame.waveLength);
            if (frame.waveLength > 0) {
                sourceWave.read(chunk);
            }
            if ((size_t)frame.waveLength < srcBytes) {
                std::fill(srcBuffer.begin() + frame.waveLength, srcBuffer.end(), 0);
            }
        }

        if (destChannels == srcChannels) {
            if (fileIn.isAudioTrackPlanned && sourceDualMonoChannel >= 0) {
                // mergeの目標が2chでも、元デュアルモノ区間は指定言語だけを両側へ渡す。
                auto* samples = reinterpret_cast<int16_t*>(srcBuffer.data());
                for (int sample = 0; sample < frameSamples; sample++) {
                    const int16_t value = samples[sample * srcChannels + sourceDualMonoChannel];
                    samples[sample * srcChannels] = samples[sample * srcChannels + 1] = value;
                }
            }
            if (fwrite(srcBuffer.data(), srcBytes, 1, fp.get()) != 1) {
                THROWF(IOException, "Whisper入力wav作成: 書き込みに失敗 (%s)", wavPath.c_str());
            }
        } else {
            dstBuffer.resize((size_t)frameSamples * bytesPerSample);
            const int channelIndex = (sourceDualMonoChannel >= 0 && sourceDualMonoChannel < srcChannels) ? sourceDualMonoChannel : 0;
            const int16_t* srcSamples = reinterpret_cast<const int16_t*>(srcBuffer.data());
            int16_t* dstSamples = reinterpret_cast<int16_t*>(dstBuffer.data());
            for (int s = 0; s < frameSamples; s++) {
                if (fileIn.isAudioTrackPlanned && sourceDualMonoChannel < 0) {
                    dstSamples[s] = static_cast<int16_t>((static_cast<int>(srcSamples[s * srcChannels]) + srcSamples[s * srcChannels + 1]) / 2);
                } else {
                    dstSamples[s] = srcSamples[s * srcChannels + channelIndex];
                }
            }
            if (fwrite(dstBuffer.data(), dstBuffer.size(), 1, fp.get()) != 1) {
                THROWF(IOException, "Whisper入力wav作成: 書き込みに失敗 (%s)", wavPath.c_str());
            }
        }
    }

    return wavPath;
}

// TS解析中にtsreadexへ入力TSを転送し、トレースを保存する。
// 解析側で例外が発生した場合も、デストラクタで標準入力を閉じて完了を待つ。
class TsReadExPipe {
    class TraceProcess : public EventBaseSubProcess {
    public:
        TraceProcess(const tstring& args, File* output)
            : EventBaseSubProcess(args)
            , output_(output) {}

    protected:
        void onOut(bool isErr, MemoryChunk mc) override {
            if (!isErr && output_) {
                output_->write(mc);
            }
        }

    private:
        File* output_;
    };

public:
    TsReadExPipe(AMTContext& ctx, const ConfigWrapper& setting)
        : output_(new File(setting.getTmpTsReadExDumpPath(), _T("wb"))) {
        ctx.info(_T("[tsreadex 解析]"));
        const tstring args = StringFormat(
            _T("\"%s\" -n -1 -r - -"), setting.getTsReadExPath().c_str());
        ctx.infoF(_T("tsreadex コマンド: %s"), args.c_str());
        process_.reset(new TraceProcess(args, output_.get()));
        try {
            worker_ = std::thread([this]() { run(); });
        } catch (...) {
            // 構築失敗時も起動済みプロセスと出力回収スレッドを先に終了する。
            process_->join();
            throw;
        }
    }

    ~TsReadExPipe() {
        try {
            join();
        } catch (...) {
        }
    }

    void write(MemoryChunk mc) {
        std::unique_lock<std::mutex> lock(mutex_);
        space_.wait(lock, [this]() { return pending_ < BUFFER_COUNT || failure_; });
        if (failure_) std::rethrow_exception(failure_);
        // 先読み領域は次のreadで再利用されるため、上限付きリングへコピーする。
        auto& buffer = buffers_[tail_];
        buffer.assign(mc.data, mc.data + mc.length);
        tail_ = (tail_ + 1) % BUFFER_COUNT;
        pending_++;
        lock.unlock();
        ready_.notify_one();
    }

    int join() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable()) worker_.join();
        // processのjoinは一度だけ実行し、デストラクタからの再呼出しも許す。
        if (!joined_) {
            joined_ = true;
            exitCode_ = process_->join();
        }
        if (failure_) std::rethrow_exception(failure_);
        return exitCode_;
    }

private:
    void run() {
        try {
            bool writeEnabled = true;
            while (true) {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this]() { return pending_ != 0 || finished_; });
                if (pending_ == 0) return;
                auto& buffer = buffers_[head_];
                lock.unlock();
                if (writeEnabled) {
                    try {
                        process_->write(MemoryChunk(buffer.data(), buffer.size()));
                    } catch (const RuntimeException&) {
                        // 早期終了時は従来どおりTS解析を継続し、joinの終了コードで判定する。
                        writeEnabled = false;
                    }
                }
                lock.lock();
                head_ = (head_ + 1) % BUFFER_COUNT;
                pending_--;
                lock.unlock();
                space_.notify_one();
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                failure_ = std::current_exception();
            }
            space_.notify_one();
        }
    }

    static constexpr size_t BUFFER_COUNT = 4;
    std::unique_ptr<File> output_;
    std::unique_ptr<TraceProcess> process_;
    std::array<std::vector<uint8_t>, BUFFER_COUNT> buffers_;
    std::mutex mutex_;
    std::condition_variable ready_, space_;
    size_t head_ = 0, tail_ = 0, pending_ = 0;
    bool finished_ = false;
    bool joined_ = false;
    int exitCode_ = 0;
    std::exception_ptr failure_;
    std::thread worker_;
};

} // namespace

static ResumeDatInfo makeResumeDatSampleForTest() {
    ResumeDatInfo info;
    info.srcFileSize = 0x0102030405060708LL;
    info.srcWriteTime = 132539328000000000LL;
    info.requestedServiceId = 0x0A0B0C0D;
    info.captionsParsed = true;
    info.tsreplace = false;
    info.chapter = true;
    info.pmtCut = false;
    info.pmtCutSideRate[0] = 1.5;
    info.pmtCutSideRate[1] = -2.25;
    const std::string logoFilenameUtf8("\xE3\x83\xAD\xE3\x82\xB4\xF0\x9F\x98\x80.lgd", 14);
    const tstring logoFilename = resumeDatUtf8ToTString(logoFilenameUtf8);
    ResumeDatLogoEntry logoEntry;
    logoEntry.filename = logoFilename;
    for (size_t index = 0; index < logoEntry.sha256.size(); index++) {
        logoEntry.sha256[index] = static_cast<uint8_t>(index);
    }
    info.logoTable.push_back(logoEntry);
    info.detectionLogoIndices.push_back(0);
    info.ignoreNoLogo = true;
    info.noLogoInCM = false;
    info.noDelogo = true;
    info.looseLogoDetection = false;
    info.autoLogoDetect = -101;
    info.autoLogoDetectSearchFrames = 202;
    info.autoLogoDetectDivX = 303;
    info.autoLogoDetectDivY = 404;
    info.autoLogoDetectBlockSize = 505;
    info.autoLogoDetectThreshold = 606;
    info.autoLogoDetectMarginX = 707;
    info.autoLogoDetectMarginY = -808;
    info.serviceId = 0x11223344;
    info.numTotalPackets = 0x0102030405060708LL;
    info.numScramblePackets = -2;
    info.totalIntVideoSize = 123456789;
    info.splitterSrcFileSize = info.srcFileSize;
    info.noDrcsMapCount = 17;
    ResumeDatVideoInfo video;
    video.numFrames = 1234;
    video.selectedLogoIndex = 0;
    video.logoFrameBytes = { 0x00, 0x7F, 0x80, 0xFF };
    info.videos.push_back(std::move(video));
    return info;
}

extern "C" AMATSUKAZE_API int WriteResumeDatSampleForTest(const tchar* path) {
    if (path == nullptr) return -1;
    try {
        File file(path, _T("wb"));
        writeResumeDatInfo(file, makeResumeDatSampleForTest());
        file.flush();
        return 0;
    } catch (...) {
        return -3;
    }
}

extern "C" AMATSUKAZE_API int ReadResumeDatManifestForTest(
    const tchar* path,
    char* settingsLogoFilenameUtf8,
    const size_t settingsLogoFilenameCapacity,
    char* videoLogoFilenameUtf8,
    const size_t videoLogoFilenameCapacity,
    int64_t* sourceFileSize,
    int64_t* sourceWriteTimeFileTime,
    int32_t* requestedServiceId,
    int32_t* serviceId,
    int32_t* videoFrameCount,
    uint8_t* logoFrameBytes,
    const size_t logoFrameCapacity,
    size_t* logoFrameSize) {
    if (path == nullptr || settingsLogoFilenameUtf8 == nullptr || videoLogoFilenameUtf8 == nullptr
        || sourceFileSize == nullptr || sourceWriteTimeFileTime == nullptr || requestedServiceId == nullptr
        || serviceId == nullptr || videoFrameCount == nullptr || logoFrameBytes == nullptr || logoFrameSize == nullptr) {
        return -1;
    }
    try {
        const ResumeDatInfo info = readResumeDatManifest(path);
        const std::string settingsFilename = info.detectionLogoIndices.empty()
            ? std::string()
            : resumeDatTStringToUtf8(info.logoTable[info.detectionLogoIndices.front()].filename);
        const ResumeDatVideoInfo* video = info.videos.empty() ? nullptr : &info.videos.front();
        const std::string videoFilename = video == nullptr || video->selectedLogoIndex < 0
            ? std::string()
            : resumeDatTStringToUtf8(info.logoTable[static_cast<size_t>(video->selectedLogoIndex)].filename);
        const size_t payloadSize = video == nullptr ? 0 : video->logoFrameBytes.size();
        if (settingsFilename.size() + 1 > settingsLogoFilenameCapacity
            || videoFilename.size() + 1 > videoLogoFilenameCapacity
            || payloadSize > logoFrameCapacity) {
            return -2;
        }
        std::memcpy(settingsLogoFilenameUtf8, settingsFilename.c_str(), settingsFilename.size() + 1);
        std::memcpy(videoLogoFilenameUtf8, videoFilename.c_str(), videoFilename.size() + 1);
        if (payloadSize != 0) {
            std::memcpy(logoFrameBytes, video->logoFrameBytes.data(), payloadSize);
        }
        *sourceFileSize = info.srcFileSize;
        *sourceWriteTimeFileTime = info.srcWriteTime;
        *requestedServiceId = info.requestedServiceId;
        *serviceId = info.serviceId;
        *videoFrameCount = video == nullptr ? -1 : video->numFrames;
        *logoFrameSize = payloadSize;
        return 0;
    } catch (...) {
        return 1;
    }
}

extern "C" AMATSUKAZE_API int RoundTripResumeDatForTest(
    const tchar* sourcePath, const tchar* outputPath) {
    if (sourcePath == nullptr || outputPath == nullptr) return -1;
    try {
        const ResumeDatInfo info = readResumeDatManifest(sourcePath);
        File output(outputPath, _T("wb"));
        writeResumeDatInfo(output, info);
        output.flush();
        return 0;
    } catch (...) {
        return 1;
    }
}

extern "C" AMATSUKAZE_API int RebaseResumeDatLogoPathsForTest(
    const tchar* const* savedLogoFilenames,
    const size_t savedLogoFilenameCount,
    const tchar* const* currentLogoPaths,
    const size_t currentLogoPathCount,
    const tchar* const* videoLogoFilenames,
    const size_t videoLogoFilenameCount,
    const tchar* eraseLogoPath,
    char* outputUtf8,
    const size_t outputUtf8Capacity,
    size_t* outputUtf8Length) {
    if (eraseLogoPath == nullptr || outputUtf8 == nullptr || outputUtf8Length == nullptr
        || savedLogoFilenameCount > static_cast<size_t>(INT_MAX)
        || currentLogoPathCount > static_cast<size_t>(INT_MAX)
        || videoLogoFilenameCount > static_cast<size_t>(INT_MAX)
        || (savedLogoFilenameCount != 0 && savedLogoFilenames == nullptr)
        || (currentLogoPathCount != 0 && currentLogoPaths == nullptr)
        || (videoLogoFilenameCount != 0 && videoLogoFilenames == nullptr)) {
        return -1;
    }
    try {
        ResumeDatInfo info;
        info.serviceId = 123;
        std::vector<tstring> currentPaths;
        auto addLogoTableEntry = [&](const tstring& filename) -> uint32_t {
            for (uint32_t index = 0; index < info.logoTable.size(); index++) {
                if (info.logoTable[index].filename == filename) return index;
                if (resumeDatLogoNamesCollide(info.logoTable[index].filename, filename)) {
                    THROW(FormatException, "Resume.dat test fixture has a case-only basename collision");
                }
            }
            ResumeDatLogoEntry entry;
            entry.filename = filename;
            info.logoTable.push_back(std::move(entry));
            return static_cast<uint32_t>(info.logoTable.size() - 1);
        };
        for (size_t index = 0; index < savedLogoFilenameCount; index++) {
            if (savedLogoFilenames[index] == nullptr) return -1;
            info.detectionLogoIndices.push_back(addLogoTableEntry(savedLogoFilenames[index]));
        }
        for (size_t index = 0; index < currentLogoPathCount; index++) {
            if (currentLogoPaths[index] == nullptr) return -1;
            currentPaths.emplace_back(currentLogoPaths[index]);
        }
        for (size_t index = 0; index < videoLogoFilenameCount; index++) {
            if (videoLogoFilenames[index] == nullptr) return -1;
            ResumeDatVideoInfo video;
            const tstring filename(videoLogoFilenames[index]);
            if (!filename.empty()) {
                video.selectedLogoIndex = static_cast<int32_t>(addLogoTableEntry(filename));
            }
            info.videos.push_back(std::move(video));
        }
        if (*eraseLogoPath != 0) info.eraseLogoIndices.push_back(0);

        tstring reason;
        if (!rebaseResumeDatLogoPaths(info, currentPaths, reason)) return 1;
        std::string output;
        auto appendPath = [&](const tstring& path) {
            if (!output.empty()) output.push_back(static_cast<char>(10));
            output += resumeDatTStringToUtf8(path);
        };
        for (const auto& path : currentPaths) appendPath(path);
        for (const auto& video : info.videos) appendPath(video.logoPath);
        *outputUtf8Length = output.size();
        if (output.size() + 1 > outputUtf8Capacity) return -2;
        std::memcpy(outputUtf8, output.c_str(), output.size() + 1);
        return 0;
    } catch (...) {
        return 1;
    }
}

extern "C" AMATSUKAZE_API int ValidateResumeDatLogoFilenameForTest(
    const tchar* filename, const size_t filenameLength) {
    if (filename == nullptr && filenameLength != 0) return -1;
    try {
        const tstring value = filenameLength == 0 ? tstring() : tstring(filename, filenameLength);
        return isSafeResumeDatLogoFilename(value) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}
AMTSplitter::AMTSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, true, isCaptionParsingEnabled(setting))
    , setting_(setting)
    , psWriter(ctx)
    , writeHandler(*this)
    , audioFile_(setting.getAudioFilePath(), _T("wb"))
    , waveFile_(setting.getWaveFilePath(), _T("wb"))
    , curVideoFormat_()
    , videoFileCount_(0)
    , videoStreamType_(-1)
    , audioStreamType_(-1)
    , audioFileSize_(0)
    , waveFileSize_(0)
    , srcFileSize_(0) {
    psWriter.setHandler(&writeHandler);
    if (setting.getMode() == _T("cm") && GetProcessorCount() > 1) {
        audioPipeline_.reset(new AudioPesPipeline(ctx,
            [this](int audioIdx, int64_t clock, const std::vector<AudioFrameData>& frames, PESPacket packet, const AudioComponentInfo& component) {
                auto& format = asyncAudioFormats_[audioIdx];
                if (frames[0].format != format) {
                    format = frames[0].format;
                    onAudioFormatChanged(audioIdx, format);
                }
                onAudioPesPacket(audioIdx, clock, frames, packet, component);
            }));
    }
}

AMTSplitter::~AMTSplitter() {
    // 他のメンバーが破棄される前に、未反映イベントを捨ててワーカーを回収する。
    audioPipeline_.reset();
}

void AMTSplitter::drainAudio() {
    if (audioPipeline_) audioPipeline_->drain();
}

bool AMTSplitter::onAudioPesReady(int audioIdx, int64_t clock, PESPacket packet, const AudioComponentInfo& component) {
    if (!audioPipeline_) return false;
    audioPipeline_->submit(audioIdx, clock, packet, component);
    return true;
}

StreamReformInfo AMTSplitter::split() {
    readAll();
    drainAudio();

    // for debug
    printInteraceCount();

    return StreamReformInfo(ctx, videoFileCount_,
        videoFrameList_, audioFrameList_, captionTextList_, streamEventList_, timeList_, std::move(captionPesList_));
}

int64_t AMTSplitter::getSrcFileSize() const {
    return srcFileSize_;
}

int64_t AMTSplitter::getTotalIntVideoSize() const {
    return writeHandler.getTotalSize();
}
AMTSplitter::StreamFileWriteHandler::StreamFileWriteHandler(TsSplitter& this_)
    : this_(this_), totalIntVideoSize_() {}
/* virtual */ void AMTSplitter::StreamFileWriteHandler::onStreamData(MemoryChunk mc) {
    if (file_ != NULL) {
        file_->write(mc);
        totalIntVideoSize_ += mc.length;
    }
}
void AMTSplitter::StreamFileWriteHandler::open(const tstring& path) {
    totalIntVideoSize_ = 0;
    file_ = std::unique_ptr<File>(new File(path, _T("wb")));
}
void AMTSplitter::StreamFileWriteHandler::close() {
    file_ = nullptr;
}
int64_t AMTSplitter::StreamFileWriteHandler::getTotalSize() const {
    return totalIntVideoSize_;
}

void AMTSplitter::readAll() {
    enum {
        BUFSIZE = 4 * 1024 * 1024,
        BUFFER_COUNT = 4
    };
    ReadAheadFile srcfile(setting_.getSrcFilePath(), BUFSIZE, BUFFER_COUNT);
    // tsreplaceで一時TSを使う場合だけ、入力TSのコピーを作成する。
    const bool needCopyTS = setting_.getFormat() == FORMAT_TSREPLACE
        && setting_.isMuxTsTempEnabled();
    std::unique_ptr<File> rawts;
    if (needCopyTS) {
        rawts.reset(new File(setting_.getTmpRawTSPath(), _T("wb")));
    }
    std::unique_ptr<TsReadExPipe> tsreadex;
    if (isTsReadExAvailable(setting_)) {
        tsreadex.reset(new TsReadExPipe(ctx, setting_));
    }
    srcFileSize_ = srcfile.size();
    while (true) {
        const MemoryChunk chunk = srcfile.read();
        if (chunk.length == 0) break;
        if (rawts) {
            rawts->write(chunk);
        }
        if (tsreadex) {
            tsreadex->write(chunk);
        }
        inputTsData(chunk);
    }
    if (tsreadex) {
        const int exitCode = tsreadex->join();
        if (exitCode != 0) {
            THROWF(FormatException, "tsreadexがエラーコード(%d)を返しました", exitCode);
        }
    }
}

/* static */ bool AMTSplitter::CheckPullDown(PICTURE_TYPE p0, PICTURE_TYPE p1) {
    switch (p0) {
    case PIC_TFF:
    case PIC_BFF_RFF:
        return (p1 == PIC_TFF || p1 == PIC_TFF_RFF);
    case PIC_BFF:
    case PIC_TFF_RFF:
        return (p1 == PIC_BFF || p1 == PIC_BFF_RFF);
    default: // それ以外はチェック対象外
        return true;
    }
}

void AMTSplitter::printInteraceCount() {

    if (videoFrameList_.size() == 0) {
        ctx.error(_T("フレームがありません"));
        return;
    }

    // ラップアラウンドしないPTSを生成
    std::vector<std::pair<int64_t, int>> modifiedPTS;
    int64_t videoBasePTS = videoFrameList_[0].PTS;
    int64_t prevPTS = videoFrameList_[0].PTS;
    for (int i = 0; i < int(videoFrameList_.size()); i++) {
        int64_t PTS = videoFrameList_[i].PTS;
        int64_t modPTS = prevPTS + int64_t((int32_t(PTS) - int32_t(prevPTS)));
        modifiedPTS.emplace_back(modPTS, i);
        prevPTS = modPTS;
    }

    // PTSでソート
    std::sort(modifiedPTS.begin(), modifiedPTS.end());

#if 0
    // フレームリストを出力
    FILE* framesfp = fopen("frames.txt", "w");
    fprintf(framesfp, "FrameNumber,DecodeFrameNumber,PTS,Duration,FRAME_TYPE,PIC_TYPE,IsGOPStart\n");
    for (int i = 0; i < (int)modifiedPTS.size(); i++) {
        int64_t PTS = modifiedPTS[i].first;
        int decodeIndex = modifiedPTS[i].second;
        const VideoFrameInfo& frame = videoFrameList_[decodeIndex];
        int PTSdiff = -1;
        if (i < (int)modifiedPTS.size() - 1) {
            int64_t nextPTS = modifiedPTS[i + 1].first;
            const VideoFrameInfo& nextFrame = videoFrameList_[modifiedPTS[i + 1].second];
            PTSdiff = int(nextPTS - PTS);
            if (CheckPullDown(frame.pic, nextFrame.pic) == false) {
                ctx.warnF(_T("Flag Check Error: PTS=%lld %s -> %s"),
                    PTS, PictureTypeString(frame.pic), PictureTypeString(nextFrame.pic));
            }
        }
        fprintf(framesfp, "%d,%d,%lld,%d,%s,%s,%d\n",
            i, decodeIndex, PTS, PTSdiff, FrameTypeString(frame.type), PictureTypeString(frame.pic), frame.isGopStart ? 1 : 0);
    }
    fclose(framesfp);
#endif

    // PTS間隔を出力
    struct Integer {
        int v;
        Integer() : v(0) {}
    };

    std::array<int, MAX_PIC_TYPE> interaceCounter = { 0 };
    std::map<int, Integer> PTSdiffMap;
    prevPTS = -1;
    for (const auto& ptsIndex : modifiedPTS) {
        int64_t PTS = ptsIndex.first;
        const VideoFrameInfo& frame = videoFrameList_[ptsIndex.second];
        interaceCounter[(int)frame.pic]++;
        if (prevPTS != -1) {
            int PTSdiff = int(PTS - prevPTS);
            PTSdiffMap[PTSdiff].v++;
        }
        prevPTS = PTS;
    }

    ctx.info(_T("[映像フレーム統計情報]"));

    int64_t totalTime = modifiedPTS.back().first - videoBasePTS;
    double sec = (double)totalTime / MPEG_CLOCK_HZ;
    int minutes = (int)(sec / 60);
    sec -= minutes * 60;
    ctx.infoF(_T("時間: %d分%.3f秒"), minutes, sec);

    ctx.infoF(_T("FRAME=%d DBL=%d TLP=%d TFF=%d BFF=%d TFF_RFF=%d BFF_RFF=%d"),
        interaceCounter[0], interaceCounter[1], interaceCounter[2], interaceCounter[3], interaceCounter[4], interaceCounter[5], interaceCounter[6]);

    for (const auto& pair : PTSdiffMap) {
        ctx.infoF(_T("(PTS_Diff,Cnt)=(%d,%d)"), pair.first, pair.second.v);
    }
}

// TsSplitter仮想関数 //

/* virtual */ void AMTSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    if (audioPipeline_ && audioPipeline_->hasPending()) {
        auto data = std::make_shared<std::vector<uint8_t>>(packet.data, packet.data + packet.length);
        auto ownedFrames = std::make_shared<std::vector<VideoFrameInfo>>(frames);
        audioPipeline_->defer([this, clock, data, ownedFrames]() {
            PESPacket ownedPacket(MemoryChunk(data->data(), data->size()));
            ownedPacket.parse();
            commitVideoPes(clock, *ownedFrames, ownedPacket);
        }, data->size() + frames.size() * sizeof(VideoFrameInfo));
        return;
    }
    commitVideoPes(clock, frames, packet);
}

void AMTSplitter::commitVideoPes(int64_t clock, const std::vector<VideoFrameInfo>& frames, PESPacket packet) {
    for (const VideoFrameInfo& frame : frames) {
        videoFrameList_.push_back(frame);
        videoFrameList_.back().fileOffset = writeHandler.getTotalSize();
    }
    psWriter.outVideoPesPacket(clock, frames, packet);
}

/* virtual */ void AMTSplitter::onVideoFormatChanged(VideoFormat fmt) {
    drainAudio();
    ctx.info(_T("[映像フォーマット変更]"));

    StringBuilder sb;
    sb.append("サイズ: %dx%d", fmt.width, fmt.height);
    if (fmt.width != fmt.displayWidth || fmt.height != fmt.displayHeight) {
        sb.append(" 表示領域: %dx%d", fmt.displayWidth, fmt.displayHeight);
    }
    int darW, darH; fmt.getDAR(darW, darH);
    sb.append(" (%d:%d)", darW, darH);
    if (fmt.fixedFrameRate) {
        sb.append(" FPS: %d/%d", fmt.frameRateNum, fmt.frameRateDenom);
    } else {
        sb.append(" FPS: VFR");
    }
    ctx.info(char_to_tstring(sb.str()));

    // ファイル変更
    if (!curVideoFormat_.isBasicEquals(fmt)) {
        // アスペクト比以外も変更されていたらファイルを分ける
        //（StreamReformと条件を合わせなければならないことに注意）
        writeHandler.open(setting_.getIntVideoFilePath(videoFileCount_++));
        psWriter.outHeader(videoStreamType_, audioStreamType_);
    }
    curVideoFormat_ = fmt;

    StreamEvent ev = StreamEvent();
    ev.type = VIDEO_FORMAT_CHANGED;
    ev.frameIdx = (int)videoFrameList_.size();
    streamEventList_.push_back(ev);
}

/* virtual */ void AMTSplitter::onAudioPesPacket(
    int audioIdx,
    int64_t clock,
    const std::vector<AudioFrameData>& frames,
    PESPacket packet, const AudioComponentInfo& component) {
    for (const AudioFrameData& frame : frames) {
        FileAudioFrameInfo info = frame;
        info.audioIdx = audioIdx;
        info.component = component;
        info.codedDataSize = frame.codedDataSize;
        info.waveDataSize = frame.decodedDataSize;
        info.fileOffset = audioFileSize_;
        info.waveOffset = waveFileSize_;
        audioFile_.write(MemoryChunk(frame.codedData, frame.codedDataSize));
        if (frame.decodedDataSize > 0) {
            waveFile_.write(MemoryChunk((uint8_t*)frame.decodedData, frame.decodedDataSize));
        }
        audioFileSize_ += frame.codedDataSize;
        waveFileSize_ += frame.decodedDataSize;
        audioFrameList_.push_back(info);
    }
    if (videoFileCount_ > 0) {
        psWriter.outAudioPesPacket(audioIdx, clock, frames, packet);
    }
}

/* virtual */ void AMTSplitter::onAudioFormatChanged(int audioIdx, AudioFormat fmt) {
    ctx.infoF(_T("[音声%dフォーマット変更]"), audioIdx);
    ctx.infoF(_T("チャンネル: %s サンプルレート: %d"),
        getAudioChannelString(fmt.channels), fmt.sampleRate);

    StreamEvent ev = StreamEvent();
    ev.type = AUDIO_FORMAT_CHANGED;
    ev.audioIdx = audioIdx;
    ev.frameIdx = (int)audioFrameList_.size();
    streamEventList_.push_back(ev);
}

/* virtual */ void AMTSplitter::onCaptionPesPacket(
    int64_t clock,
    std::vector<CaptionItem>& captions,
    PESPacket packet) {
    drainAudio();
    for (auto& caption : captions) {
        captionTextList_.emplace_back(std::move(caption));
    }
}

// 管理データを含め、描画用デコーダに必要な全字幕PESを保持する。
void AMTSplitter::onRawCaptionPesPacket(int64_t PTS, MemoryChunk payload) {
    drainAudio();
    CaptionPesItem item;
    item.PTS = PTS;
    item.data.assign(payload.data, payload.data + payload.length);
    captionPesList_.push_back(std::move(item));
}

/* virtual */ DRCSOutInfo AMTSplitter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    drainAudio();
    DRCSOutInfo info;
    info.elapsed = (videoFrameList_.size() > 0) ? (double)(PTS - videoFrameList_[0].PTS) : -1.0;
    info.filename = setting_.getDRCSOutPath(md5);
    return info;
}

// TsPacketSelectorHandler仮想関数 //

/* virtual */ void AMTSplitter::onPidTableChanged(const PMTESInfo video, const std::vector<PMTESInfo>& audio, const PMTESInfo caption) {
    drainAudio();
    // ベースクラスの処理
    TsSplitter::onPidTableChanged(video, audio, caption);

    // 音声のない TS もある。ASSERT はリリースビルドでは無効なので、空のまま audio[0] を読まない
    videoStreamType_ = video.stype;
    audioStreamType_ = audio.empty() ? -1 : audio[0].stype;

    StreamEvent ev = StreamEvent();
    ev.type = PID_TABLE_CHANGED;
    ev.numAudio = (int)audio.size();
    ev.frameIdx = (int)videoFrameList_.size();
    streamEventList_.push_back(ev);
}

/* virtual */ void AMTSplitter::onTime(int64_t clock, JSTTime time) {
    drainAudio();
    timeList_.push_back(std::make_pair(clock, time));
}


/* static */ tstring replaceOptions(
    const tstring& options,
    const VideoFormat& fmt,
    const ConfigWrapper& setting,
    const EncodeFileKey key,
    const int serviceID) {
    tstring ret = options;
    ret = str_replace(ret, _T("@IMAGE_WIDTH@"), StringFormat(_T("%d"), fmt.width));
    ret = str_replace(ret, _T("@IMAGE_HEIGHT@"), StringFormat(_T("%d"), fmt.height));
    ret = str_replace(ret, _T("@SERVICE_ID@"), StringFormat(_T("%d"), serviceID));
    ret = str_replace(ret, _T("@AMT_ENCODER@"), encoderToString(setting.getEncoder()));
    ret = str_replace(ret, _T("@AMT_AUDIO_ENCODER@"), audioEncoderToString(setting.getAudioEncoder()));
    ret = str_replace(ret, _T("@AMT_TEMP_DIR@"), setting.getTmpDir());
    ret = str_replace(ret, _T("@AMT_TEMP_VIDEO@"), _T("\"") + setting.getEncVideoFilePath(key) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_AUDIO@"), _T("\"") + setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder()) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_AUDIO_0@"), _T("\"") + setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder()) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_AUDIO_1@"), _T("\"") + setting.getIntAudioFilePath(key, 1, setting.getAudioEncoder()) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_CHAPTER@"), _T("\"") + setting.getTmpChapterPath(key) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_TIMECODE@"), _T("\"") + setting.getAvsTimecodePath(key) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS@"), _T("\"") + setting.getTmpASSFilePath(key, 0) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_0@"), _T("\"") + setting.getTmpASSFilePath(key, 0) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_1@"), _T("\"") + setting.getTmpASSFilePath(key, 1) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_SRT@"), _T("\"") + setting.getTmpSRTFilePath(key, 0) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_SRT_0@"), _T("\"") + setting.getTmpSRTFilePath(key, 0) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_SRT_1@"), _T("\"") + setting.getTmpSRTFilePath(key, 1) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_NICOJK_720S@"), _T("\"") + setting.getTmpNicoJKASSPath(key, NICOJK_720S) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_NICOJK_720T@"), _T("\"") + setting.getTmpNicoJKASSPath(key, NICOJK_720T) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_NICOJK_1080S@"), _T("\"") + setting.getTmpNicoJKASSPath(key, NICOJK_1080S) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_NICOJK_1080T@"), _T("\"") + setting.getTmpNicoJKASSPath(key, NICOJK_1080T) + _T("\""));
    return ret;
}

void AddEnvironmentVariable(std::wstring& envBlock, const std::wstring& name, const std::wstring& value) {
    envBlock += name;
    envBlock += L"=";
    envBlock += value;
    envBlock += L'\0';
}

tstring GetDirectoryName(const tstring& path) {
    size_t lastSeparator = path.find_last_of(_T("/\\"));
    if (lastSeparator == tstring::npos) {
        return _T("");  // ディレクトリ区切りが見つからない場合は空文字列を返す
    }
    return path.substr(0, lastSeparator);
}

/* static */ int executeBatchFile(
    const tstring& batchPath,
    const VideoFormat& fmt,
    const ConfigWrapper& setting,
    const EncodeFileKey key,
    const int serviceID) {
    if (batchPath.empty()) {
        return 0;
    }
    
    // 一時バッチファイルのパスを生成
    tstring tempBatchPath = setting.getEncVideoFilePath(key) + _T(".") + rgy_get_extension(batchPath);
    
    try {
        // バッチファイルをコピー
        if (rgy_file_copy(batchPath.c_str(), tempBatchPath.c_str(), true) == 0) {
            return -1;
        }

        const auto outPath = StringFormat(_T("%s.%s"), setting.getOutFileBaseWithoutPrefix().c_str(), setting.getOutputExtention(setting.getFormat()));

        SetTemporaryEnvironmentVariable tmpvar;
        tmpvar.set(_T("CLI_IN_PATH"), setting.getSrcFilePath().c_str());
        tmpvar.set(_T("TS_IN_PATH"), setting.getSrcFileOriginalPath().c_str());
        tmpvar.set(_T("SERVICE_ID"), StringFormat(_T("%d"), serviceID).c_str());
        tmpvar.set(_T("CLI_OUT_PATH"), outPath);
        tmpvar.set(_T("TS_IN_DIR"), GetDirectoryName(setting.getSrcFileOriginalPath().c_str()));
        tmpvar.set(_T("CLI_OUT_DIR"), GetDirectoryName(outPath));
        tmpvar.set(_T("OUT_DIR"), GetDirectoryName(outPath));
        tmpvar.set(_T("IMAGE_WIDTH"), StringFormat(_T("%d"), fmt.width));
        tmpvar.set(_T("IMAGE_HEIGHT"), StringFormat(_T("%d"), fmt.height));
        tmpvar.set(_T("SERVICE_ID"), StringFormat(_T("%d"), serviceID));
        tmpvar.set(_T("AMT_ENCODER"), encoderToString(setting.getEncoder()));
        tmpvar.set(_T("AMT_AUDIO_ENCODER"), audioEncoderToString(setting.getAudioEncoder()));
        tmpvar.set(_T("AMT_TEMP_DIR"), setting.getTmpDir());
        tmpvar.set(_T("AMT_TEMP_AVS"), setting.getAvsTmpPath(key));
        tmpvar.set(_T("AMT_TEMP_AVS_TC"), setting.getAvsTimecodePath(key));
        tmpvar.set(_T("AMT_TEMP_AVS_DURATION"), setting.getAvsDurationPath(key));
        tmpvar.set(_T("AMT_TEMP_AFS_TC"), setting.getAfsTimecodePath(key));
        tmpvar.set(_T("AMT_TEMP_VIDEO"), setting.getEncVideoFilePath(key));
        tmpvar.set(_T("AMT_TEMP_AUDIO"), setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder()));
        tmpvar.set(_T("AMT_TEMP_AUDIO_0"), setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder()));
        tmpvar.set(_T("AMT_TEMP_AUDIO_1"), setting.getIntAudioFilePath(key, 1, setting.getAudioEncoder()));
        tmpvar.set(_T("AMT_TEMP_CHAPTER"), setting.getTmpChapterPath(key));
        tmpvar.set(_T("AMT_TEMP_TIMECODE"), setting.getAvsTimecodePath(key));
        tmpvar.set(_T("AMT_TEMP_ASS"), setting.getTmpASSFilePath(key, 0));
        tmpvar.set(_T("AMT_TEMP_ASS_0"), setting.getTmpASSFilePath(key, 0));
        tmpvar.set(_T("AMT_TEMP_ASS_1"), setting.getTmpASSFilePath(key, 1));
        tmpvar.set(_T("AMT_TEMP_SRT"), setting.getTmpSRTFilePath(key, 0));
        tmpvar.set(_T("AMT_TEMP_SRT_0"), setting.getTmpSRTFilePath(key, 0));
        tmpvar.set(_T("AMT_TEMP_SRT_1"), setting.getTmpSRTFilePath(key, 1));
        tmpvar.set(_T("AMT_TEMP_ASS_NICOJK_720S"), setting.getTmpNicoJKASSPath(key, NICOJK_720S));
        tmpvar.set(_T("AMT_TEMP_ASS_NICOJK_720T"), setting.getTmpNicoJKASSPath(key, NICOJK_720T));
        tmpvar.set(_T("AMT_TEMP_ASS_NICOJK_1080S"), setting.getTmpNicoJKASSPath(key, NICOJK_1080S));
        tmpvar.set(_T("AMT_TEMP_ASS_NICOJK_1080T"), setting.getTmpNicoJKASSPath(key, NICOJK_1080T));

        // バッチファイルを実行
        std::unique_ptr<RGYPipeProcess> process = createRGYPipeProcess();
        // 標準入出力は不要なので全て無効化
        process->init(PIPE_MODE_DISABLE, PIPE_MODE_DISABLE, PIPE_MODE_DISABLE);
        std::vector<tstring> args = { tempBatchPath };
        int runResult = process->run(args, setting.getTmpDir().c_str(), 0, false, true, true); // 最小化で実行
        if (runResult != 0) {
            return -1;
        }
        // プロセスの終了を待機し、終了コードを取得
        int exitCode = process->waitAndGetExitCode();
        return exitCode;
        
    } catch (...) {
        return -1;
    }
}

EncoderArgumentGenerator::EncoderArgumentGenerator(
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo)
    : setting_(setting)
    , reformInfo_(reformInfo) {}

tstring EncoderArgumentGenerator::GenEncoderOptions(
    int numFrames,
    VideoFormat outfmt,
    std::vector<BitrateZone> zones,
    double vfrBitrateScale,
    tstring timecodepath,
    int vfrTimingFps,
    EncodeFileKey key, int pass, int serviceID,
    const EncoderOptionInfo& eoInfo,
    const tstring& outPathOverride,
    bool chunkIsCM) {
    VIDEO_STREAM_FORMAT srcFormat = reformInfo_.getVideoStreamFormat();
    double srcBitrate = getSourceBitrate(key.video);
    const tstring outPath = (outPathOverride.size() > 0)
        ? outPathOverride
        : setting_.getEncVideoFilePath(key);
    return makeEncoderArgs(
        setting_.getEncoder(),
        setting_.getEncoderPath(),
        replaceOptions(setting_.getOptions(
            numFrames, srcFormat, srcBitrate, false, pass, zones,
            setting_.getEncVideoOptionFilePath(key), vfrBitrateScale, key, eoInfo, chunkIsCM),
            outfmt, setting_, key, serviceID),
        outfmt,
        timecodepath,
        vfrTimingFps,
        setting_.getFormat(),
        outPath,
        setting_.getSARInContainerOnly());
}

// src, target
std::pair<double, double> EncoderArgumentGenerator::printBitrate(AMTContext& ctx, EncodeFileKey key) const {
    double srcBitrate = getSourceBitrate(key.video);
    ctx.infoF(_T("入力映像ビットレート: %d kbps"), (int)srcBitrate);
    VIDEO_STREAM_FORMAT srcFormat = reformInfo_.getVideoStreamFormat();
    double targetBitrate = std::numeric_limits<float>::quiet_NaN();
    if (setting_.isAutoBitrate()) {
        targetBitrate = setting_.getBitrate().getTargetBitrate(srcFormat, srcBitrate);
        if (key.cm == CMTYPE_CM) {
            targetBitrate *= setting_.getBitrateCM();
        }
        ctx.infoF(_T("目標映像ビットレート: %d kbps"), (int)targetBitrate);
    }
    return std::make_pair(srcBitrate, targetBitrate);
}

double EncoderArgumentGenerator::getSourceBitrate(int fileId) const {
    // ビットレート計算
    const auto& info = reformInfo_.getSrcVideoInfo(fileId);
    return ((double)info.first * 8 / 1000) / ((double)info.second / MPEG_CLOCK_HZ);
}

// 各ゾーンに、本エンコーダのタイムライン上の表示時刻を設定する
// エンコーダフィルタが別プロセスの場合、フレーム番号ではずれるため時刻でゾーンを指定する
// Amatsukazeはy4mをヘッダFPSのCFRとして書き出し(Y4MWriterはFRAME行に時刻を付加しない)、
// エンコーダフィルタはその時刻軸のままXts=で本エンコーダへ伝えるため、通常はフレーム番号/FPSでよい
// ただしAVS由来のVFRタイムコードがある場合はCFR換算ではずれるため、実時刻をそのまま使う
static void FillBitrateZoneTimes(
    std::vector<BitrateZone>& zones,
    const VideoInfo& outvi,
    const std::vector<double>& timeCodes) {
    const double tick = (double)outvi.fps_denominator / outvi.fps_numerator;
    const auto frameToSec = [&](int frame) -> double {
        if (frame >= 0 && frame < (int)timeCodes.size()) {
            return timeCodes[frame] / 1000.0;
        }
        return frame * tick;
    };
    for (auto& zone : zones) {
        zone.startSec = frameToSec(zone.startFrame);
        zone.endSec = frameToSec(zone.endFrame);
    }
}

/* static */ std::vector<BitrateZone> MakeBitrateZones(
    const std::vector<double>& timeCodes,
    const std::vector<EncoderZone>& cmzones,
    const ConfigWrapper& setting,
    const EncoderOptionInfo& eoInfo,
    VideoInfo outvi) {
    std::vector<BitrateZone> bitrateZones;
    if (timeCodes.size() == 0 || setting.isEncoderSupportVFR()) {
        // VFRでない、または、エンコーダがVFRをサポートしている -> VFR用に調整する必要がない
        for (int i = 0; i < (int)cmzones.size(); i++) {
            bitrateZones.emplace_back(cmzones[i], setting.getBitrateCM(), setting.getCMQualityOffset());
        }
    } else {
        if (setting.isZoneAvailable()) {
            // VFR非対応エンコーダでゾーンに対応していればビットレートゾーン生成
#if 0
            {
                File dump("zone_param.dat", "wb");
                dump.writeArray(frameDurations);
                dump.writeArray(cmzones);
                dump.writeValue(setting.getBitrateCM());
                dump.writeValue(outvi.fps_numerator);
                dump.writeValue(outvi.fps_denominator);
                dump.writeValue(setting.getX265TimeFactor());
                dump.writeValue(0.05);
            }
#endif
            if (auto rcMode = getRCMode(setting.getEncoder(), eoInfo.rcMode);
                setting.isZoneWithQualityAvailable()   // 品質オフセットを--dynamic-rcで指定可能なエンコーダである
                && !setting.isAutoBitrate()            // 自動ビットレートでない
                && rcMode && !rcMode->isBitrateMode    // ビットレートモードでない
                && setting.getCMQualityOffset() != 0.0 // 品質オフセットが有効
            ) {
                for (int i = 0; i < (int)cmzones.size(); i++) {
                    bitrateZones.emplace_back(cmzones[i], setting.getBitrateCM(), setting.getCMQualityOffset());
                }
            } else {
                bitrateZones = MakeVFRBitrateZones(
                    timeCodes, cmzones, setting.getBitrateCM(),
                    outvi.fps_numerator, outvi.fps_denominator,
                    setting.getX265TimeFactor(), 0.05); // 全体で5%までの差なら許容する
            }
        }
    }
    if (setting.isZoneTimeBased()) {
        FillBitrateZoneTimes(bitrateZones, outvi, timeCodes);
    }
    return bitrateZones;
}

// ページヒープが機能しているかテスト
#if 0
void DoBadThing() {
    char *p = (char*)HeapAlloc(
        GetProcessHeap(),
        HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY,
        8);
    memset(p, 'x', 32);
}
#endif

/* static */ void transcodeMain(AMTContext& ctx, const ConfigWrapper& setting) {
#if 0
    MessageBox(NULL, "Debug", "Amatsukaze", MB_OK);
    //DoBadThing();
#endif

    auto thSetPowerThrottling = std::make_unique<RGYThreadSetPowerThrottoling>(GetCurrentProcessId());
    thSetPowerThrottling->run(RGYThreadPowerThrottlingMode::Disabled);

    const_cast<ConfigWrapper&>(setting).CreateTempDir();
    setting.dump();

    bool isNoEncode = (setting.getMode() == _T("cm"));

    auto eoInfo = ParseEncoderOption(setting.getEncoder(), setting.getEncoderOptions());
    ctx.info(_T("[本エンコーダ設定]"));
    PrintEncoderInfo(ctx, eoInfo);
    if (setting.isEncoderFilterEnabled()) {
        const tstring& filterOptions = setting.getEncoderFilterOptions();
        if (filterOptions.find(_T("--output-res")) != tstring::npos
            || filterOptions.find(_T("--crop")) != tstring::npos
            || filterOptions.find(_T("--vpp-pad")) != tstring::npos) {
            ctx.warn(_T("エンコーダフィルタによる解像度変更はコンテナのメタデータ (解像度/SAR) に反映されません"));
        }
    }
    if (setting.isEncoderFilterSeparate()) {
        // エンコーダフィルタのオプション文字列は解析しない。
        // インタレース解除の有無はGUI設定から明示的に受け取り、
        // フレームレート・フレーム数の変化はエンコード後に実測タイムコードから判定する。
        if (eoInfo.deint != ENCODER_DEINT_NONE && setting.isEncoderFilterDeinterlace()) {
            THROW(ArgumentException, "本エンコーダとエンコーダフィルタの両方でインタレース解除が指定されています");
        }
    }
    const int cliParallel = setting.getEncoderParallel();
    const int encoderParallel = (cliParallel > 1) ? cliParallel : ((eoInfo.parallel > 1) ? eoInfo.parallel : 1);
    if (setting.isEncoderFilterSeparate() && encoderParallel > 1 && !isSoftwareSplitEncoder(setting.getEncoder())) {
        THROW(ArgumentException, "別プロセスのエンコーダフィルタとネイティブ分割並列エンコードは併用できません。フィルタがフレーム数を変更するとchunk-handlesの開始フレームが本エンコーダの出力フレーム位置と一致しないためです");
    }
    if (setting.isTwoPass() && encoderParallel > 1) {
        THROW(ArgumentException, "2passエンコード時は分割エンコードを使用できません (--enc-parallel / --parallel は無効です)");
    }
    if (setting.isEncoderFilterSeparate() && setting.isTwoPass()) {
        ctx.warn(_T("エンコーダフィルタを別プロセスで使用する2passエンコードでは、フィルタ処理も2回実行されます"));
    }

    // チェック
    if (!isNoEncode && eoInfo.afsTimecode && !setting.isFormatVFRSupported()) {
        THROW(FormatException, "M2TS/TS出力はVFRをサポートしていません");
    }
    if (!isNoEncode && eoInfo.afsTimecode && setting.isEncoderFilterSeparate()) {
        // 別プロセスのエンコーダフィルタ出力へ本エンコーダでさらにVFR化を適用すると、
        // 両段のタイムライン変更を正しく統合できない。
        THROW(ArgumentException,
            "エンコーダフィルタと本エンコーダの両方でVFR化することはできません");
    }
    if (setting.getFormat() == FORMAT_TSREPLACE) {
        auto cmtypes = setting.getCMTypes();
        const bool supportedSingleOutput = cmtypes.size() == 1;
        const bool supportedSplitOutput = cmtypes.size() == 2
            && cmtypes[0] == CMTYPE_NONCM && cmtypes[1] == CMTYPE_CM;
        if (!supportedSingleOutput && !supportedSplitOutput) {
            THROW(FormatException, "tsreplaceの出力選択が不正です");
        }
        if (eoInfo.format != VS_H264 && eoInfo.format != VS_H265 && eoInfo.format != VS_MPEG2) {
            THROW(FormatException, "tsreplaceはH.264/H.265/MPEG-2以外には対応していません");
        }
    }

    ResourceManger rm(ctx, setting.getInPipe(), setting.getOutPipe());
    rm.wait(HOST_CMD_TSAnalyze);

    Stopwatch sw;
    sw.start();
    ResumeInfo resumeInfo;
    std::unique_ptr<StreamReformInfo> reformInfoPtr;
    const bool isReusingTmp = !isNoEncode && tryLoadResume(ctx, setting, resumeInfo, reformInfoPtr);
    const bool captionsParsed = isReusingTmp ? resumeInfo.captionsParsed : isCaptionParsingEnabled(setting);
    int serviceId;
    int64_t numTotalPackets;
    int64_t numScramblePackets;
    int64_t totalIntVideoSize;
    int64_t srcFileSize;
    int noDrcsMapCount;
    if (isReusingTmp) {
        serviceId = resumeInfo.serviceId;
        numTotalPackets = resumeInfo.numTotalPackets;
        numScramblePackets = resumeInfo.numScramblePackets;
        totalIntVideoSize = resumeInfo.totalIntVideoSize;
        srcFileSize = resumeInfo.splitterSrcFileSize;
        noDrcsMapCount = resumeInfo.noDrcsMapCount;
    } else {
        auto splitter = std::unique_ptr<AMTSplitter>(new AMTSplitter(ctx, setting));
        if (setting.getServiceId() > 0) {
            splitter->setServiceId(setting.getServiceId());
        }
        reformInfoPtr = std::make_unique<StreamReformInfo>(splitter->split());
        ctx.infoF(_T("TS解析完了: %.2f秒"), sw.getAndReset());
        if (captionsParsed && !setting.isSubtitlesEnabled()) {
            ctx.info(_T("[一時ファイル再利用] 再開情報保存用に字幕を解析しました（字幕処理は無効のため出力しません）"));
        }
        serviceId = splitter->getActualServiceId();
        numTotalPackets = splitter->getNumTotalPackets();
        numScramblePackets = splitter->getNumScramblePackets();
        totalIntVideoSize = splitter->getTotalIntVideoSize();
        srcFileSize = splitter->getSrcFileSize();
        noDrcsMapCount = ctx.getErrorCount(AMT_ERR_NO_DRCS_MAP);
    }
    StreamReformInfo& reformInfo = *reformInfoPtr;

    if (!isReusingTmp && setting.isNoRemoveTmp()) {
        saveResumeStreamInfo(ctx, setting, reformInfo);
    }
    if (!isReusingTmp && captionsParsed && !setting.isSubtitlesEnabled()) {
        reformInfo.clearCaptionItems();
    }

    if (setting.isDumpStreamInfo()) {
        reformInfo.serialize(setting.getStreamInfoPath());
    }

    // スクランブルパケットチェック
    double scrambleRatio = (double)numScramblePackets / (double)numTotalPackets;
    if (scrambleRatio > 0.01) {
        ctx.errorF(_T("%.2f%%のパケットがスクランブル状態です。"), scrambleRatio * 100);
        if (scrambleRatio > 0.3) {
            THROW(FormatException, "スクランブルパケットが多すぎます");
        }
    }

    if (!isNoEncode && setting.isIgnoreNoDrcsMap() == false) {
        // DRCSマッピングチェック
        if ((setting.isSubtitlesEnabled() ? noDrcsMapCount : 0) > 0) {
            THROW(NoDrcsMapException, "マッピングにないDRCS外字あり正常に字幕処理できなかったため終了します");
        }
    }

    if (!isReusingTmp) {
        reformInfo.prepare(setting.isSplitSub(), setting.isEncodeAudio(), setting.getFormat() == FORMAT_TSREPLACE,
            setting.getAudioFormatChangeMode(), setting.getAudioFilePath());
    }
    if (setting.isMpeg2PartialEnabled()) {
        if (reformInfo.getVideoStreamFormat() != VS_MPEG2) {
            THROW(FormatException, "--mpeg2-partialの入力映像はMPEG-2である必要があります");
        }
        for (int videoFileIndex = 0; videoFileIndex < reformInfo.getNumVideoFile(); videoFileIndex++) {
            const auto& videoFormat = reformInfo.getFormat(EncodeFileKey(videoFileIndex, 0)).videoFormat;
            if (videoFormat.format != VS_MPEG2 || !videoFormat.fixedFrameRate) {
                THROW(FormatException, "--mpeg2-partialは固定フレームレートのMPEG-2映像だけに対応しています");
            }
        }
    }

    time_t startTime = reformInfo.getFirstFrameTime();

    NicoJK nicoJK(ctx, setting);
    bool nicoOK = false;
    if (!isNoEncode && setting.isNicoJKEnabled()) {
        ctx.info(_T("[ニコニコ実況コメント取得]"));
        auto srcDuration = reformInfo.getInDuration() / MPEG_CLOCK_HZ;
        nicoOK = nicoJK.makeASS(serviceId, startTime, (int)srcDuration);
        if (nicoOK) {
            reformInfo.SetNicoJKList(nicoJK.getDialogues());
        } else {
            if (nicoJK.isFail() == false) {
                ctx.info(_T("対応チャンネルがありません"));
            } else if (setting.isIgnoreNicoJKError() == false) {
                THROW(RuntimeException, "ニコニコ実況コメント取得に失敗");
            }
        }
    }

    int numVideoFiles = reformInfo.getNumVideoFile();
    int mainFileIndex = reformInfo.getMainVideoFileIndex();
    ResumeDatInfo resumeDatInfo;
    const bool isReusingResumeDat = !isReusingTmp && tryLoadResumeDat(ctx, setting,
        resumeDatInfo, reformInfo, serviceId, numTotalPackets, numScramblePackets,
        totalIntVideoSize, srcFileSize);
    std::vector<std::unique_ptr<CMAnalyze>> cmanalyze;

    // ソースファイル読み込み用データ保存
    if (isReusingTmp) {
        ctx.info(_T("[一時ファイル再利用] 現在の設定でソースファイル読み込み用データを再生成します"));
    }
    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; videoFileIndex++) {
        // ファイル読み込み情報を保存
        auto& fmt = reformInfo.getFormat(EncodeFileKey(videoFileIndex, 0));
        auto amtsPath = setting.getTmpAMTSourcePath(videoFileIndex);
        ctx.infoF(_T("ソースファイル読み込み用データ保存[%d/%d]: %s"), videoFileIndex + 1, numVideoFiles, amtsPath.c_str());
        const auto& filterAudioFrames = reformInfo.getFilterSourceAudioFrames(videoFileIndex);
        const auto& sourceAudioFrames = reformInfo.getAudioFrameList();
        std::vector<uint8_t> primaryAudioFrames;
        primaryAudioFrames.reserve(filterAudioFrames.size());
        for (const auto& frame : filterAudioFrames) {
            primaryAudioFrames.push_back(frame.frameIndex >= 0
                && sourceAudioFrames[frame.frameIndex].format.channels == AUDIO_2LANG ? 1 : 0);
        }
        av::SaveAMTSource(amtsPath,
            setting.getIntVideoFilePath(videoFileIndex),
            setting.getWaveFilePath(),
            fmt.videoFormat, fmt.audioFormat[0],
            reformInfo.getFilterSourceFrames(videoFileIndex),
            filterAudioFrames,
            setting.getDecoderSetting(), primaryAudioFrames);
        ctx.infoF(_T("ソースファイル読み込み用データ保存完了[%d/%d]"), videoFileIndex + 1, numVideoFiles);
    }

    // ロゴ・CM解析
    if (!isReusingTmp) {
        rm.wait(HOST_CMD_CMAnalyze);
        if (isReusingResumeDat) {
            ctx.info(_T("[Resume.dat再利用] 候補を通常のロゴ解析位置で照合します"));
        } else {
            ctx.infoF(_T("[ロゴ・CM解析]"));
        }
        sw.start();
    } else if (isReusingTmp) {
        ctx.info(_T("[一時ファイル再利用] ロゴ・CM解析結果を再利用します"));
    }
    struct LogoMatchState {
        size_t frames;
        bool found;
        tstring message;
    };
    std::vector<LogoMatchState> logoFound;
    std::vector<std::unique_ptr<MakeChapter>> chapterMakers(numVideoFiles);
    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; videoFileIndex++) {
        cmanalyze.push_back(std::make_unique<CMAnalyze>(ctx, setting));
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        const bool delogoEnabled = setting.isNoDelogo() ? false : true;
        // チャプター解析は300フレーム（約10秒）以上ある場合だけ
        //（短すぎるとエラーになることがあるので
        const bool analyzeChapterAndCM = (setting.isChapterEnabled() && numFrames >= 300);
        CMAnalyze *cma = cmanalyze.back().get();
        if (isReusingTmp) {
            const auto& resumeVideo = resumeInfo.videos[videoFileIndex];
            cma->restore(resumeVideo.logoPath, resumeVideo.trims, resumeVideo.divs, numFrames);
        } else {
            if (isReusingResumeDat) {
                cma->setResumeDatLogoReuse(makeResumeDatLogoReuseData(resumeDatInfo, videoFileIndex));
            }
            const auto& inputVideofmt = reformInfo.getFormat(EncodeFileKey(videoFileIndex, 0)).videoFormat;
            if (analyzeChapterAndCM || delogoEnabled) {
                cma->analyze(serviceId, videoFileIndex, inputVideofmt, numFrames, analyzeChapterAndCM);
            }

            if (analyzeChapterAndCM && setting.isPmtCutEnabled()) {
                // PMT変更によるCM追加認識
                cma->applyPmtCut(numFrames, setting.getPmtCutSideRate(),
                    reformInfo.getPidChangedList(videoFileIndex));
            }
        }

        if (videoFileIndex == mainFileIndex) {
            if (setting.getTrimAVSPath().size()) {
                // Trim情報入力
                cma->inputTrimAVS(numFrames, setting.getTrimAVSPath());
            }
            if (setting.getDivFilePath().size()) {
                // 分割点情報入力
                cma->inputDivFile(numFrames, setting.getDivFilePath());
            }
        }

        logoFound.push_back(LogoMatchState{
            (size_t)numFrames,
            cma->getLogoPath().size() > 0,
            cma->getLogoMatchFailMessage() });
        reformInfo.applyCMZones(videoFileIndex, cma->getZones(), cma->getDivs());

        if (analyzeChapterAndCM) {
            chapterMakers[videoFileIndex] = std::unique_ptr<MakeChapter>(
                new MakeChapter(ctx, setting, reformInfo, videoFileIndex, cma->getTrims()));
        }
    }
    if (setting.isChapterEnabled()) {
        // ロゴがあったかチェック //
        // 映像ファイルをフレーム数でソート
        std::sort(logoFound.begin(), logoFound.end(), [](const LogoMatchState& a, const LogoMatchState& b) {
            return a.frames < b.frames;
        });
        const bool logoRequired = !setting.isNoDelogo()
            || (setting.isChapterEnabled() && !setting.isNoLogoInCM());
        if (!logoFound.empty() &&
            setting.getLogoPath().size() > 0 && // ロゴ指定あり
            logoRequired &&
            setting.isIgnoreNoLogo() == false &&          // ロゴなし無視でない
            logoFound.back().frames >= 300 &&
            logoFound.back().found == false)     // 最も長い映像でロゴが見つからなかった
        {
            const auto& longest = logoFound.back();
            const tstring message = longest.message.empty()
                ? tstring(_T("マッチするロゴが見つかりませんでした"))
                : longest.message;
            throw_exception_(NoLogoException(StringFormat(
                _T("Exception thrown at %s:%d\r\nMessage: %s"),
                core_utils::file_name_t(__FILENAME__).c_str(), __LINE__, message.c_str())));
        }
        ctx.infoF(_T("ロゴ・CM解析完了: %.2f秒"), sw.getAndReset());
    }

    if (setting.isNoRemoveTmp()) {
        saveResumeFiles(ctx, setting, reformInfo, cmanalyze,
            serviceId, numTotalPackets, numScramblePackets, totalIntVideoSize, srcFileSize,
            noDrcsMapCount, captionsParsed);
    }
    const bool resumeDatLogoCacheMiss = std::any_of(cmanalyze.begin(), cmanalyze.end(),
        [](const auto& analysis) { return analysis->didResumeDatLogoCacheMiss(); });
    if (!isReusingTmp && (!isReusingResumeDat || resumeDatLogoCacheMiss)) {
        saveResumeDatSidecar(ctx, setting, reformInfo, cmanalyze,
            serviceId, numTotalPackets, numScramblePackets, totalIntVideoSize, srcFileSize,
            noDrcsMapCount, captionsParsed);
    }

    int cmCutOffsetFrames = setting.getCmCutOffsetFrames();
    if (cmCutOffsetFrames > 0 && !setting.getTrimAVSPath().empty()) {
        ctx.info(_T("trim.avsからカット点を読み込むため、CMカット点の自動オフセットを無効にしました"));
        cmCutOffsetFrames = 0;
    }

    if (isNoEncode) {
        if (setting.isCopyTrimAVSEnabled()) {
            copyCutInfoForCMOnly(ctx, setting, reformInfo, cmanalyze, numVideoFiles);
        }
        if (setting.isOutputChapterEnabled()) {
            // 入力TS用のチャプターなので、CMを含む全フレームを補正なしで保持する。
            reformInfo.genAudio({ CMTYPE_BOTH }, 0);
            ctx.info(_T("[チャプター生成]"));
            for (const auto& key : reformInfo.getOutFileKeys()) {
                const auto& fileIn = reformInfo.getEncodeFile(key);
                if (fileIn.duration >= (int64_t)setting.getMinOutputDuration() * MPEG_CLOCK_HZ
                    && chapterMakers[key.video]) {
                    chapterMakers[key.video]->exec(key);
                    const auto path = setting.getTmpChapterPath(key);
                    if (File::exists(path)) {
                        // CM解析のみでは-o(出力先)は使わず、入力ファイル名からチャプターの出力先を組み立てる
                        const auto dstchapter = setting.getCMOnlyChapterPath(fileIn.outKey, fileIn.keyMax);
                        if (!rgy_file_copy(path, dstchapter, true)) {
                            THROWF(IOException, "チャプターファイルの出力に失敗しました: %s", dstchapter.c_str());
                        }
                        ctx.infoF(_T("[CM解析のみ] チャプターを出力: %s"), dstchapter.c_str());
                    }
                }
            }
        }
        return; // CM解析のみならここで終了
    }

    auto audioDiffInfo = reformInfo.genAudio(setting.getCMTypes(), cmCutOffsetFrames);
    audioDiffInfo.printAudioPtsDiff(ctx);

    const auto& allKeys = reformInfo.getOutFileKeys();
    std::vector<EncodeFileKey> keys;
    // 指定秒数未満の短い区間は出力しない
    const int64_t minOutputDuration = (int64_t)setting.getMinOutputDuration() * MPEG_CLOCK_HZ;
    std::copy_if(allKeys.begin(), allKeys.end(), std::back_inserter(keys),
        [&](EncodeFileKey key) { return reformInfo.getEncodeFile(key).duration >= minOutputDuration; });

    std::vector<EncodeFileOutput> outFileInfo(keys.size());

    ctx.info(_T("[チャプター生成]"));
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];
        if (chapterMakers[key.video]) {
            chapterMakers[key.video]->exec(key);
        }
    }

    std::vector<WhisperAudioEntry> whisperAudioEntries;
    struct WhisperTask {
        int keyIndex;
        EncodeFileKey key;
        int localIndex;
        WhisperProcessParam param;
        tstring srtPath;
        tstring vttPath;
        std::unique_ptr<StdRedirectedSubProcess> process;
        int exitCode = -1; // join()の結果 (-1: 未実行)
        tstring errorMessage;
    };
    std::vector<WhisperTask> whisperTasks;
    std::vector<int> whisperLocalIndex(keys.size(), 0);
    if (setting.isEncodeAudio()) {
        ctx.info(_T("[音声エンコード]"));
        for (int i = 0; i < (int)keys.size(); i++) {
            auto key = keys[i];
            auto outpath = setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder());
            auto args = makeAudioEncoderArgs(
                setting.getAudioEncoder(),
                setting.getAudioEncoderPath(),
                setting.getAudioEncoderOptions(),
                setting.getAudioBitrateInKbps(),
                outpath);
            auto format = reformInfo.getFormat(key);
            auto audioFrames = reformInfo.getWaveInput(reformInfo.getEncodeFile(key).audioFrames[0]);
            EncodeAudio(ctx, args, setting.getWaveFilePath(), format.audioFormat[0], audioFrames);
            whisperAudioEntries.push_back({ i, key, 0, outpath, 0, -1 });
        }
    } else if (setting.getFormat() != FORMAT_TSREPLACE
        || (setting.getSubtitleMode() == SUBMODE_WHISPER_ALWAYS || setting.getSubtitleMode() == SUBMODE_WHISPER_FALLBACK)) { // tsreplaceの場合は音声ファイルを作らない
        ctx.info(_T("[音声出力]"));
        PacketCache audioCache(ctx, setting.getAudioFilePath(), reformInfo.getAudioFileOffsets(), 12, 4);
        for (int i = 0; i < (int)keys.size(); i++) {
            const auto key = keys[i];
            const auto& fileIn = reformInfo.getEncodeFile(key);
            const auto fmt = reformInfo.getFormat(key);
            if (fileIn.isAudioTrackPlanned) {
                for (int adst = 0; adst < (int)fileIn.audioTrackPlan.size(); adst++) {
                    const auto& track = fileIn.audioTrackPlan[adst];
                    const auto filepath = setting.getIntAudioFilePath(key, adst, setting.getAudioEncoder());
                    BuildAudioTrack(ctx, audioCache, track, filepath, reformInfo.getAudioFrameList());
                    // Whisperも出力プランの無音区間とデュアルモノ展開を反映する。
                    const int dualMonoChannel = track.layout == AUDIO_MONO ? track.logicalTrack : -1;
                    whisperAudioEntries.push_back({ i, key, adst, filepath, track.sourceTrack, dualMonoChannel });
                    ctx.infoF(_T("音声%d-%d (%s) 出力 -> %s"), fileIn.outKey.format, adst, track.name.c_str(), filepath.c_str());
                }
                continue;
            }
            for (int asrc = 0, adst = 0; asrc < (int)fileIn.audioFrames.size(); asrc++) {
                const std::vector<int>& frameList = fileIn.audioFrames[asrc];
                if (frameList.size() > 0) {
                    const bool isDualMono = (fmt.audioFormat[asrc].channels == AUDIO_2LANG);
                    if (!setting.isEncodeAudio() && isDualMono) {
                        // デュアルモノは2つのAACに分離
                        ctx.infoF(_T("音声%d-%dはデュアルモノなので2つのAACファイルに分離します"), fileIn.outKey.format, asrc);
                        const int adst0 = adst++;
                        const int adst1 = adst++;
                        SpDualMonoSplitter splitter(ctx);
                        const tstring filepath0 = setting.getIntAudioFilePath(key, adst0, setting.getAudioEncoder());
                        whisperAudioEntries.push_back({ i, key, adst0, filepath0, asrc, 0 });
                        const tstring filepath1 = setting.getIntAudioFilePath(key, adst1, setting.getAudioEncoder());
                        whisperAudioEntries.push_back({ i, key, adst1, filepath1, asrc, 1 });
                        splitter.open(0, filepath0);
                        splitter.open(1, filepath1);
                        for (int frameIndex : frameList) {
                            splitter.inputPacket(audioCache[frameIndex]);
                        }
                        ctx.infoF(_T("音声%d-%d[0]出力 -> %s"), fileIn.outKey.format, asrc, filepath0.c_str());
                        ctx.infoF(_T("音声%d-%d[1]出力 -> %s"), fileIn.outKey.format, asrc, filepath1.c_str());
                    } else {
                        if (isDualMono) {
                            ctx.infoF(_T("音声%d-%dはデュアルモノですが、音声フォーマット無視指定があるので分離しません"), fileIn.outKey.format, asrc);
                        }
                        const int adst0 = adst++;
                        const tstring filepath = setting.getIntAudioFilePath(key, adst0, setting.getAudioEncoder());
                        whisperAudioEntries.push_back({ i, key, adst0, filepath, asrc, -1 });
                        File file(filepath, _T("wb"));
                        for (int frameIndex : frameList) {
                            file.write(audioCache[frameIndex]);
                        }
                        ctx.infoF(_T("音声%d-%d出力 -> %s"), fileIn.outKey.format, asrc, filepath.c_str());
                    }
                }
            }
        }
    }

    std::vector<PsisiarcTask> psisiarcTasks;
    ctx.info(_T("[字幕ファイル生成]"));
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];
        CaptionASSFormatter formatterASS(ctx);
        CaptionSRTFormatter formatterSRT(ctx);
        NicoJKFormatter formatterNicoJK(ctx);
        const auto& capList = reformInfo.getEncodeFile(key).captionList;
        for (int lang = 0; lang < (int)capList.size(); lang++) {
            auto ass = formatterASS.generate(capList[lang]);
            auto srt = formatterSRT.generate(capList[lang]);
            WriteUTF8File(setting.getTmpASSFilePath(key, lang), ass);
            ctx.infoF(_T("字幕ファイル出力: %s"), setting.getTmpASSFilePath(key, lang).c_str());
            if (srt.size() > 0) {
                // SRTはCP_STR_SMALLしかなかった場合など出力がない場合があり、
                // 空ファイルはmux時にエラーになるので、1行もない場合は出力しない
                WriteUTF8File(setting.getTmpSRTFilePath(key, lang), srt);
                ctx.infoF(_T("字幕ファイル出力: %s"), setting.getTmpSRTFilePath(key, lang).c_str());
            }
        }
        if (nicoOK) {
            const auto& headerLines = nicoJK.getHeaderLines();
            const auto& dialogues = reformInfo.getEncodeFile(key).nicojkList;
            for (NicoJKType jktype : setting.getNicoJKTypes()) {
                File file(setting.getTmpNicoJKASSPath(key, jktype), _T("w"));
                auto text = formatterNicoJK.generate(headerLines[(int)jktype], dialogues[(int)jktype]);
                file.write(MemoryChunk((uint8_t*)text.data(), text.size()));
            }
        }
        // 字幕構築 + (必要なら) WebVTT生成
        try {
            if (setting.isWebVTTEnabled()) {
                reformInfo.genWebVTT(key, setting, psisiarcTasks);
            }
        } catch (const Exception& e) {
            ctx.warnF(_T("WebVTT生成に失敗: %s"), e.message());
        }

        // Whisperによる字幕生成 (モード制御 + 複数音声)
        try {
            if (setting.getSubtitleMode() == SUBMODE_WHISPER_ALWAYS || setting.getSubtitleMode() == SUBMODE_WHISPER_FALLBACK) {
                SubtitleGenerator whisperGen(ctx);
                const auto wdir    = setting.getTmpWhisperDir();
                const auto whisper = setting.getWhisperPath();
                // 追加オプション組み立て
                StringBuilderT opt;
                if (setting.getWhisperModel().size() > 0) opt.append(_T("--model %s"), setting.getWhisperModel());
                if (setting.getWhisperOption().size() > 0) {
                    if (opt.str().size() > 0) opt.append(_T(" "));
                    opt.append(_T("%s"), setting.getWhisperOption());
                }
                const tstring extraOpt = opt.str();
                const bool fallbackOnly = (setting.getSubtitleMode() == SUBMODE_WHISPER_FALLBACK);
                const bool haveArib = (reformInfo.getEncodeFile(key).captionList.size() > 0);
                const bool shouldRun = (!fallbackOnly) || (fallbackOnly && !haveArib);
                if (shouldRun) {
                    const bool isWhisperCpp = exeIsWhisperCpp(whisper);
                    for (const auto& entry : whisperAudioEntries) {
                        if (entry.keyIndex != i) {
                            continue;
                        }

                        tstring whisperInput = entry.audioPath;
                        if (isWhisperCpp && !_tcheck_ext(whisperInput.c_str(), _T(".wav"))) {
                            try {
                                tstring wavInput = createWhisperWaveInput(ctx, setting, reformInfo, entry);
                                if (!wavInput.empty()) {
                                    whisperInput = wavInput;
                                } else {
                                    ctx.warnF(_T("whisper-cpp用のwav生成に失敗したため、元の音声を使用します: %s"), whisperInput.c_str());
                                }
                            } catch (const Exception& e) {
                                ctx.warnF(_T("whisper-cpp用wav生成中に例外: %s"), e.message());
                                ctx.warnF(_T("元の音声を使用します: %s"), whisperInput.c_str());
                            }
                        }

                        if (whisperInput.empty()) {
                            ctx.warn(_T("Whisper字幕生成: 音声入力パスが空のためスキップします"));
                            continue;
                        }
                        const tstring outFileWithoutExt = setting.getTmpWhisperFilenameWithoutExt(entry.key, entry.localIndex);
                        const auto srtPath = setting.getTmpWhisperSrtPath(entry.key, entry.localIndex);
                        const auto vttPath = setting.getTmpWhisperVttPath(entry.key, entry.localIndex);

                        if (setting.isWhisperParallelEnabled()) {
                            // エンコードと並列実行: ここではタスクだけ登録し、別スレッドで直列実行する
                            WhisperTask task;
                            task.keyIndex = i;
                            task.key = entry.key;
                            task.localIndex = entry.localIndex;
                            task.param.whisperPath = whisper;
                            task.param.audioPath = whisperInput;
                            task.param.outDir = wdir;
                            task.param.outFileWithoutExt = outFileWithoutExt;
                            task.param.extraOptions = extraOpt;
                            task.param.enableVtt = setting.isWebVTTEnabled();
                            task.param.isUtf8Log = true;
                            task.param.captureOnly = true;
                            task.srtPath = srtPath;
                            task.vttPath = vttPath;
                            task.process = nullptr;
                            whisperTasks.push_back(std::move(task));
                        } else {
                            // 従来どおり同期実行（構造体で一括指定）
                            WhisperProcessParam param;
                            param.whisperPath       = whisper;
                            param.audioPath         = whisperInput;
                            param.outDir            = wdir;
                            param.outFileWithoutExt = outFileWithoutExt;
                            param.extraOptions      = extraOpt;
                            param.enableVtt         = setting.isWebVTTEnabled();
                            param.isUtf8Log         = true;
                            param.captureOnly       = false;
                            whisperGen.runWhisper(param);
                            // 空ファイルになっていたら削除する
                            uint64_t filesize = 0;
                            if (rgy_file_exists(srtPath) && rgy_get_filesize(srtPath.c_str(), &filesize) && filesize == 0) {
                                rgy_file_remove(srtPath.c_str());
                            }
                            if (rgy_file_exists(vttPath) && rgy_get_filesize(vttPath.c_str(), &filesize) && filesize == 0) {
                                rgy_file_remove(vttPath.c_str());
                            }
                        }
                    }
                }
            }
        } catch (const Exception& e) {
            ctx.warnF(_T("Whisper字幕生成に失敗: %s"), e.message());
        }
    }
    ctx.infoF(_T("字幕ファイル生成完了: %.2f秒"), sw.getAndReset());

    auto argGen = std::unique_ptr<EncoderArgumentGenerator>(new EncoderArgumentGenerator(setting, reformInfo));

    // Whisperは対象出力のエンコード用リソースを確保してから起動し、
    // 次のリソースフェーズへ移る前に完了させる。
    std::unique_ptr<std::thread> whisperThread;
    auto hasWhisperTasks = [&](int keyIndex) {
        return setting.isWhisperParallelEnabled()
            && std::any_of(whisperTasks.begin(), whisperTasks.end(),
                [keyIndex](const WhisperTask& task) { return task.keyIndex == keyIndex; });
    };
    auto startWhisperTasks = [&](int keyIndex) {
        if (!hasWhisperTasks(keyIndex)) {
            return;
        }
        whisperThread = std::make_unique<std::thread>([&ctx, &whisperTasks, keyIndex]() {
            SubtitleGenerator whisperGen(ctx);
            for (auto& task : whisperTasks) {
                if (task.keyIndex != keyIndex) {
                    continue;
                }
                try {
                    task.process = whisperGen.startWhisperProcess(task.param);
                    task.exitCode = task.process->join();
                } catch (const Exception& e) {
                    task.errorMessage = e.message();
                } catch (...) {
                    task.errorMessage = _T("不明なエラー");
                }
            }
        });
    };
    auto finishWhisperTasks = [&](int keyIndex) {
        if (whisperThread && whisperThread->joinable()) {
            ctx.info(_T("[Whisper字幕生成: バックグラウンド処理の完了待ち]"));
            whisperThread->join();
            whisperThread.reset();
        }
        for (auto& task : whisperTasks) {
            if (task.keyIndex != keyIndex) {
                continue;
            }
            if (!task.errorMessage.empty()) {
                ctx.warnF(_T("Whisper字幕生成に失敗: %s"), task.errorMessage.c_str());
                continue;
            }
            if (!task.process) {
                continue;
            }
            const int ret = task.exitCode;

            const auto& lines = task.process->getCapturedLines();
            if (!lines.empty()) {
                ctx.info(_T("↓↓↓↓↓↓Whisper出力↓↓↓↓↓↓"));
                for (const auto& v : lines) {
                    std::vector<char> buf = v;
                    if (buf.empty() || buf.back() != '\0') {
                        buf.push_back('\0');
                    }
                    ctx.infoF(_T("%s"), char_to_tstring(buf.data()));
                }
                ctx.info(_T("↑↑↑↑↑↑Whisper出力↑↑↑↑↑↑"));
            }

            if (ret != 0) {
                ctx.warnF(_T("Whisper字幕生成に失敗 (終了コード: 0x%x)"), ret);
                continue;
            }

            // 正常終了時のみ、空SRT/VTTファイルの削除を行う
            uint64_t filesize = 0;
            if (rgy_file_exists(task.srtPath) && rgy_get_filesize(task.srtPath.c_str(), &filesize) && filesize == 0) {
                rgy_file_remove(task.srtPath.c_str());
            }
            if (rgy_file_exists(task.vttPath) && rgy_get_filesize(task.vttPath.c_str(), &filesize) && filesize == 0) {
                rgy_file_remove(task.vttPath.c_str());
            }
        }
    };
    // エンコード中の例外でunwindする場合もjoinを保証し、joinable時のterminateを防ぐ。
    struct WhisperThreadJoiner {
        std::unique_ptr<std::thread>& thread;
        ~WhisperThreadJoiner() {
            if (thread && thread->joinable()) {
                thread->join();
            }
        }
    } whisperThreadJoiner{ whisperThread };

    // psisiarcは専用スレッドでタスクを直列実行し、映像エンコードと並行させる。
    std::unique_ptr<std::thread> psisiarcThread;
    if (!psisiarcTasks.empty()) {
        psisiarcThread = std::make_unique<std::thread>([&psisiarcTasks]() {
            for (auto& task : psisiarcTasks) {
                // 例外がスレッド外へ漏れるとterminateするため、タスク単位で捕捉して継続する。
                // ログはスレッドセーフでないため出さず、同期時にまとめて報告する。
                try {
                    task.process = std::make_unique<StdRedirectedSubProcess>(
                        task.cmd, 0, false, false, true);
                    task.exitCode = task.process->join();
                } catch (const Exception& e) {
                    task.errorMessage = e.message();
                } catch (...) {
                    task.errorMessage = _T("不明なエラー");
                }
            }
        });
    }
    // エンコード中の例外でunwindする場合もjoinを保証し、joinable時のterminateを防ぐ。
    struct PsisiarcThreadJoiner {
        std::unique_ptr<std::thread>& thread;
        ~PsisiarcThreadJoiner() {
            if (thread && thread->joinable()) {
                thread->join();
            }
        }
    } psisiarcThreadJoiner{ psisiarcThread };

    sw.start();
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];
        auto& fileOut = outFileInfo[i];
        const CMAnalyze* cma = cmanalyze[key.video].get();

        if (setting.isMpeg2PartialEnabled()) {
            // フル再エンコードへのフォールバックは廃止した。失敗は例外で上位へ伝える。
            ctx.infoF(_T("[カット境界再エンコード開始] %d/%d %s"),
                i + 1, (int)keys.size(), CMTypeToString(key.cm));
            if (hasWhisperTasks(i)) {
                rm.wait(HOST_CMD_Encode);
            }
            startWhisperTasks(i);
            RunMpeg2PartialEncode(ctx, setting, reformInfo, key);
            finishWhisperTasks(i);
            const auto bitrate = argGen->printBitrate(ctx, key);
            fileOut.vfmt = reformInfo.getFormat(key).videoFormat;
            fileOut.srcBitrate = bitrate.first;
            fileOut.targetBitrate = bitrate.second;
            fileOut.vfrTimingFps = 0;
            fileOut.timecode.clear();
            fileOut.isMpeg2Partial = true;
            continue;
        }

        AMTFilterSource filterSource(ctx, setting, reformInfo,
            cma->getZones(), cma->getLogoPath(), key, rm);

        if (!setting.getPreEncBatchFile().empty()) {
            ctx.infoF(_T("[エンコード前バッチファイル] %d/%d"), i + 1, (int)keys.size());
            ctx.infoF(_T("%s"), setting.getPreEncBatchFile().c_str());
            std::unique_ptr<RGYMutex> mutexOpt;
            if (setting.isExclusiveBatExec()) {
                mutexOpt = std::make_unique<RGYMutex>("AmatsukazeCLIPreEncodeBatMutex");
            }
            {
                std::unique_ptr<RGYMutex::Guard> guard = (mutexOpt ? std::make_unique<RGYMutex::Guard>(*mutexOpt) : nullptr);
                if (executeBatchFile(setting.getPreEncBatchFile(), filterSource.getFormat(), setting, key, serviceId)) {
                    THROW(RuntimeException, "エンコード前バッチファイルの実行に失敗しました");
                }
            }
        }

        try {
            PClip filterClip = filterSource.getClip();
            IScriptEnvironment2* env = filterSource.getEnv();
            auto encoderZones = filterSource.getZones();
            auto& outfmt = filterSource.getFormat();
            auto& outvi = filterClip->GetVideoInfo();
            auto& timeCodes = filterSource.getTimeCodes();

            ctx.infoF(_T("[エンコード開始] %d/%d %s"), i + 1, (int)keys.size(), CMTypeToString(key.cm));
            auto bitrate = argGen->printBitrate(ctx, key);

            fileOut.vfmt = outfmt;
            fileOut.srcBitrate = bitrate.first;
            fileOut.targetBitrate = bitrate.second;
            fileOut.vfrTimingFps = filterSource.getVfrTimingFps();

            if (timeCodes.size() > 0) {
                // フィルタによるVFRが有効
                // エンコーダフィルタ使用時は、このタイムコードをフィルタの--tcfile-inとして渡し、
                // フィルタの出力タイムコード(--timecode)を最終的な時刻としてmuxerへ渡す
                // (フィルタがフレーム数を変えても、出力は入力の時間軸上に得られる)
                if (eoInfo.afsTimecode) {
                    THROW(ArgumentException, "エンコーダとフィルタの両方でVFRタイムコードが出力されています。");
                }
                if (eoInfo.selectEvery > 1) {
                    THROW(ArgumentException, "VFRで出力する場合は、エンコーダで間引くことはできません");
                } else if (!setting.isFormatVFRSupported()) {
                    THROW(FormatException, "M2TS/TS出力はVFRをサポートしていません");
                }
                ctx.infoF(_T("VFRタイミング: %d fps"), fileOut.vfrTimingFps);
                fileOut.timecode = setting.getAvsTimecodePath(key);
            }
            if (encoderParallel > 1) {
                ctx.infoF(_T("分割エンコード: %d"), encoderParallel);
            }

            std::vector<int> passList;
            if (setting.isTwoPass()) {
                passList.push_back(1);
                passList.push_back(2);
            } else {
                passList.push_back(-1);
            }

            auto bitrateZones = MakeBitrateZones(timeCodes, encoderZones, setting, eoInfo, outvi);
            // CMビットレート調整を、CM境界でチャンク分割することで行うか
            // ゾーン指定が使えない/使ってもフレーム番号がずれる場合に、
            // CM境界でチャンクを分けて、チャンクごとにビットレート/品質を指定する
            // ゾーンでのCM調整が使えない条件
            //  - SVT-AV1: そもそもゾーン指定がない
            //  - エンコーダフィルタ使用時: フィルタでフレーム数が変わるとゾーンのフレーム番号がずれる
            //    フィルタの内容次第では変わらないが、内容に依存した判定は取りこぼすと
            //    ずれに気づけないため、エンコーダフィルタ使用時は常にチャンク分割で対応する
            //    ただしQSVEnc/NVEncは時刻指定ゾーン(isZoneTimeBased)でずれずに指定できるため対象外
            const bool cmZoneUnusable = !setting.isZoneAvailable()
                || (setting.isEncoderFilterSeparate() && !setting.isZoneTimeBased());
            // 分割エンコードが可能な条件
            // 2passでは分割エンコードができない
            const bool canSplitEncode = isSoftwareSplitEncoder(setting.getEncoder()) && !setting.isTwoPass();
            // CM分離時はファイル単位でCM調整できるため、CMが混在するファイルのみ対象
            // (前後CMカットは中間のCMが残るため、CMを残す場合と同様に対象となる)
            const bool cmMixedInFile = (key.cm == CMTYPE_BOTH || key.cm == CMTYPE_EDGE_TRIM);
            const bool useCMChunkSplit = setting.isBitrateCMEnabled() && cmMixedInFile
                && !encoderZones.empty() && cmZoneUnusable && canSplitEncode;
            if (useCMChunkSplit) {
                ctx.info(_T("CM境界でチャンク分割してCMビットレート調整を行います"));
            }
            auto vfrBitrateScale = AdjustVFRBitrate(timeCodes, outvi.fps_numerator, outvi.fps_denominator);
            const tstring baseTimecodePath = fileOut.timecode;
            const tstring baseOutputPath = setting.getEncVideoFilePath(key);
            // x264系、x265、SVT-AV1のときはdisablePowerThrottoling=trueとする
            // QSV/NV/VCEEncではプロセス内で自動的に最適なように設定されるため不要
            const bool disablePowerThrottoling = (setting.getEncoder() == ENCODER_X264 || setting.getEncoder() == ENCODER_X262 || setting.getEncoder() == ENCODER_X265 || setting.getEncoder() == ENCODER_SVTAV1);

            AMTFilterVideoEncoder encoder(ctx, setting, std::max(4, setting.getNumEncodeBufferFrames()));
            // 並列GetFrame用にフィルタチェーンを構築するファクトリを渡す
            // 既存のfilterSourceの前処理結果（スクリプト）を再利用して環境を再構築
            auto filterFactory = [&]() -> std::unique_ptr<AMTFilterSource> {
                return std::unique_ptr<AMTFilterSource>(new AMTFilterSource(ctx, filterSource));
            };

            startWhisperTasks(i);
            encoder.encode(filterClip, outfmt,
                timeCodes, *argGen, passList, bitrateZones, encoderZones, useCMChunkSplit, vfrBitrateScale,
                baseTimecodePath, fileOut.vfrTimingFps, baseOutputPath,
                key, serviceId, eoInfo, encoderParallel, disablePowerThrottoling,
                env, filterFactory, setting.getEncoder());
            if (setting.isEncoderFilterSeparate() && File::exists(setting.getEncoderFilterTimecodePath(key))) {
                // フィルタ出力のタイムコードを実測し、CFR/VFRとフレームレートを判定する
                const tstring encoderFilterTimecodePath = setting.getEncoderFilterTimecodePath(key);
                std::vector<int> chunkBoundaries;
                const auto timestamps = readEncoderFilterTimecodeFile(encoderFilterTimecodePath, &chunkBoundaries);
                const auto tcInfo = analyzeEncoderFilterTimecode(timestamps, chunkBoundaries);
                ctx.infoF(_T("エンコーダフィルタ出力フレーム数: %d"), tcInfo.numFrames);
                if (setting.isEncoderFilterDeinterlace()) {
                    fileOut.vfmt.progressive = true;
                }
                // CFRであってもフレームレートが変化しているなら、コンテナのフレームレートを
                // 書き換えるのではなく実測タイムコードをそのまま反映する。
                // 実測比を単純な分数で近似する方式は誤差が全フレームに累積するため、
                // 番組が長いほど音ズレが大きくなるのに対し、タイムコードの反映は
                // 各フレームの時刻をそのまま渡すので誤差が累積しない。
                int fpsMul = 1, fpsDiv = 1;
                bool fpsChanged = false;
                if (!tcInfo.isVFR && tcInfo.frameDurationMs > 0.0 && fileOut.vfmt.frameRateNum > 0) {
                    const double inFrameMs = 1000.0 * fileOut.vfmt.frameRateDenom / fileOut.vfmt.frameRateNum;
                    const double ratio = inFrameMs / tcInfo.frameDurationMs;
                    if (std::abs(ratio - 1.0) > 0.001) {
                        fpsChanged = true;
                        if (approximateFpsRatio(ratio, fpsMul, fpsDiv)) {
                            ctx.infoF(_T("エンコーダフィルタによるフレームレート変化を検出しました (入力の%d/%d倍)"), fpsMul, fpsDiv);
                        } else {
                            ctx.infoF(_T("エンコーダフィルタによるフレームレート変化を検出しました (入力の%.4f倍)"), ratio);
                        }
                    }
                }

                // 入力がAVS由来のVFRの場合、フィルタ出力の間隔が一定に見えても
                // それは入力の時間軸(VFR)上の時刻なので、必ずタイムコードを反映する
                const bool inputIsVFR = timeCodes.size() > 0;
                if (tcInfo.isVFR || fpsChanged || inputIsVFR) {
                    if (setting.isFormatVFRSupported()) {
                        writeNormalizedEncoderFilterTimecode(encoderFilterTimecodePath, timestamps);
                        fileOut.timecode = encoderFilterTimecodePath;
                        // timelineeditorの丸めで16～17ms間隔が同一timestampにならないよう、
                        // encoder filterのVFR timebaseは常に120000/1001とする。
                        fileOut.vfrTimingFps = 120;
                        ctx.info(_T("エンコーダフィルタのタイムコードを整数msに正規化しました (タイムベース: 120000/1001)"));
                    } else if (tcInfo.isVFR || inputIsVFR) {
                        THROW(FormatException, "M2TS/TS出力はVFRをサポートしていません");
                    } else if (fpsMul != fpsDiv) {
                        // VFR非対応フォーマットなのでタイムコードを反映できない。
                        // 次善策としてコンテナのフレームレートを書き換える
                        fileOut.vfmt.mulDivFps(fpsMul, fpsDiv);
                        ctx.warn(_T("M2TS/TS出力ではタイムコードを反映できないため、"
                            _T("コンテナのフレームレートのみ変更します")));
                    } else {
                        ctx.warn(_T("M2TS/TS出力ではタイムコードを反映できず、"
                            _T("フレームレート変化を単純な分数でも表せないため、"
                            _T("コンテナのフレームレートは入力のままとします"))));
                    }
                }
            } else if (setting.isEncoderFilterSeparate()) {
                if (timeCodes.size() > 0) {
                    // 入力がVFRの場合、フィルタ出力のタイムコードがないと正しい時刻を復元できない
                    THROW(RuntimeException, "エンコーダフィルタがタイムコードを出力しなかったため、VFRの時刻を反映できません");
                }
                ctx.warn(_T("エンコーダフィルタがタイムコードを出力しませんでした。"
                    _T("フレームレートの変化はコンテナのメタデータに反映されません")));
            }
        } catch (const AvisynthError& avserror) {
            THROWF(AviSynthException, "%s", avserror.msg);
        }
        finishWhisperTasks(i);
    }
    ctx.infoF(_T("エンコード完了: %.2f秒"), sw.getAndReset());

    argGen = nullptr;

    // muxがpscを読む前にpsisiarcの完了を待ち、捕捉した出力をまとめて記録する。
    if (!psisiarcTasks.empty()) {
        if (psisiarcThread && psisiarcThread->joinable()) {
            ctx.info(_T("[psisiarc: バックグラウンド処理の完了待ち]"));
            psisiarcThread->join();
            psisiarcThread.reset();
        }
        for (auto& task : psisiarcTasks) {
            if (!task.errorMessage.empty()) {
                ctx.warnF(_T("psisiarcの実行に失敗: %s"), task.errorMessage.c_str());
                continue;
            }
            if (!task.process) {
                continue;
            }
            const auto& lines = task.process->getCapturedLines();
            if (!lines.empty()) {
                ctx.info(_T("↓↓↓↓↓↓psisiarc出力↓↓↓↓↓↓"));
                for (const auto& line : lines) {
                    std::vector<char> buffer = line;
                    if (buffer.empty() || buffer.back() != '\0') {
                        buffer.push_back('\0');
                    }
                    ctx.infoF(_T("%s"), char_to_tstring(buffer.data()));
                }
                ctx.info(_T("↑↑↑↑↑↑psisiarc出力↑↑↑↑↑↑"));
            }
            if (task.exitCode != 0) {
                ctx.warnF(_T("psisiarcがエラーコード(%d)を返しました"), task.exitCode);
            }
        }
    }

    // フィルタ後の表示サイズが確定してから、最終muxと同じ規則でPGSを生成する。
    if (setting.isPgsSubEnabled() && setting.isSubtitlesEnabled()) {
        Stopwatch pgsWatch;
        pgsWatch.start();
        for (int i = 0; i < (int)keys.size(); i++) {
            const auto key = keys[i];
            if (getActualOutputFormat(key, reformInfo, setting) != FORMAT_MKV) {
                continue;
            }
            const auto& format = outFileInfo[i].vfmt;
            // displayWidth/Heightはリサイズ後も入力値が残るため、最終フレームサイズを使う。
            const auto userSAR = setting.getUserSAR();
            const auto [canvasWidth, canvasHeight] = CaptionPgsCanvasSize(format.width, format.height,
                format.sarWidth, format.sarHeight, userSAR.first, userSAR.second);
            const auto& captions = reformInfo.getEncodeFile(key).captionList;
            for (int lang = 0; lang < (int)captions.size(); lang++) {
                const auto path = setting.getTmpPGSFilePath(key, lang);
                // 再開時の古いPGSを失敗後にmuxしないよう、生成前に除去する。
                if (File::exists(path)) {
                    rgy_file_remove(path.c_str());
                }
                try {
                    const auto data = GenerateCaptionPgs(reformInfo, key, lang + 1,
                        canvasWidth, canvasHeight, tchar_to_string(setting.getPgsFontFamily(), CP_UTF8),
                        [&ctx](bool warning, const std::string& message) {
                            const auto text = char_to_tstring(message, CP_UTF8);
                            if (warning) {
                                ctx.warnF(_T("PGS字幕: %s"), text.c_str());
                            } else {
                                ctx.infoF(_T("PGS字幕: %s"), text.c_str());
                            }
                        });
                    if (!data.empty()) {
                        FILE* file = fsopenT(path.c_str(), _T("wb"), _SH_DENYNO);
                        if (!file) {
                            throw std::runtime_error("PGS字幕ファイルを開けません");
                        }
                        const bool written = fwrite(data.data(), 1, data.size(), file) == data.size();
                        const bool closed = fclose(file) == 0;
                        if (!written || !closed) {
                            throw std::runtime_error("PGS字幕ファイルの書き込みに失敗しました");
                        }
                        // 成功したファイルだけをmuxへ渡し、削除できない古い出力も除外する。
                        outFileInfo[i].pgsFiles.push_back(path);
                        ctx.infoF(_T("PGS字幕出力: %s (%dx%d)"), path.c_str(), canvasWidth, canvasHeight);
                    }
                } catch (const std::exception& e) {
                    if (File::exists(path)) {
                        rgy_file_remove(path.c_str());
                    }
                    ctx.warnF(_T("PGS字幕の生成に失敗、PGSなしで続行します: %s"), char_to_tstring(e.what(), CP_UTF8));
                } catch (const Exception& e) {
                    if (File::exists(path)) {
                        rgy_file_remove(path.c_str());
                    }
                    ctx.warnF(_T("PGS字幕の生成に失敗、PGSなしで続行します: %s"), e.message());
                }
            }
        }
        ctx.infoF(_T("PGS字幕生成完了: %.2f秒"), pgsWatch.getAndReset());
    }

    rm.wait(HOST_CMD_Mux);
    sw.start();
    int64_t totalOutSize = 0;
    auto muxer = std::unique_ptr<AMTMuxder>(new AMTMuxder(ctx, setting, reformInfo));
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];

        ctx.infoF(_T("[Mux開始] %d/%d %s"), i + 1, (int)keys.size(), CMTypeToString(key.cm));
        muxer->mux(key, eoInfo, nicoOK, outFileInfo[i]);

        totalOutSize += outFileInfo[i].fileSize;
    }
    ctx.infoF(_T("Mux完了: %.2f秒"), sw.getAndReset());

    muxer = nullptr;
    thSetPowerThrottling->abortThread();

    // 出力結果を表示
    reformInfo.printOutputMapping(keys, [&](EncodeFileKey key) {
        const auto& file = reformInfo.getEncodeFile(key);
        return setting.getOutFilePath(file.outKey, file.keyMax, getActualOutputFormat(key, reformInfo, setting), eoInfo.format);
        });

    // 出力結果JSON出力
    if (setting.getOutInfoJsonPath().size() > 0) {
        StringBuilder sb;
        sb.append("{ ")
            .append("\"srcpath\": \"%s\", ", toJsonString(setting.getSrcFilePath()))
            .append("\"outfiles\": [");
        for (int i = 0; i < (int)keys.size(); i++) {
            if (i > 0) sb.append(", ");
            const auto& file = reformInfo.getEncodeFile(keys[i]);
            const auto& info = outFileInfo[i];
            sb.append("{ \"path\": \"%s\", \"srcbitrate\": %d, \"outbitrate\": %d, \"outfilesize\": %lld, ",
                toJsonString(setting.getOutFilePath(file.outKey, file.keyMax, getActualOutputFormat(keys[i], reformInfo, setting), eoInfo.format)), (int)info.srcBitrate,
                std::isnan(info.targetBitrate) ? -1 : (int)info.targetBitrate, info.fileSize);
            sb.append("\"subs\": [");
            for (int s = 0; s < (int)info.outSubs.size(); s++) {
                if (s > 0) sb.append(", ");
                sb.append("\"%s\"", toJsonString(info.outSubs[s]));
            }
            sb.append("] }");
        }
        sb.append("]")
            .append(", \"logofiles\": [");
        for (int i = 0; i < reformInfo.getNumVideoFile(); i++) {
            if (i > 0) sb.append(", ");
            sb.append("\"%s\"", toJsonString(cmanalyze[i]->getLogoPath()));
        }
        sb.append("]")
            .append(", \"srcfilesize\": %lld, \"intvideofilesize\": %lld, \"outfilesize\": %lld",
                srcFileSize, totalIntVideoSize, totalOutSize);
        auto duration = reformInfo.getInOutDuration();
        sb.append(", \"srcduration\": %.3f, \"outduration\": %.3f",
            (double)duration.first / MPEG_CLOCK_HZ, (double)duration.second / MPEG_CLOCK_HZ);
        sb.append(", \"audiodiff\": ");
        audioDiffInfo.printToJson(sb);
        sb.append(", \"error\": {");
        for (int i = 0; i < AMT_ERR_MAX; i++) {
            if (i > 0) sb.append(", ");
            sb.append("\"%s\": %d", AMT_ERROR_NAMES[i], ctx.getErrorCount((AMT_ERROR_COUNTER)i));
        }
        sb.append(" }");
        sb.append(", \"cmanalyze\": %s", (setting.isChapterEnabled() ? "true" : "false"))
            .append(", \"nicojk\": %s", (nicoOK ? "true" : "false"))
            .append(", \"trimavs\": %s", (setting.getTrimAVSPath().size() ? "true" : "false"))
            .append(" }");

        std::string str = sb.str();
        MemoryChunk mc(reinterpret_cast<uint8_t*>(const_cast<char*>(str.data())), str.size());
        File file(setting.getOutInfoJsonPath(), _T("w"));
        file.write(mc);
    }
}

/* static */ void transcodeSimpleMain(AMTContext& ctx, const ConfigWrapper& setting) {
    if (ends_with(setting.getSrcFilePath(), _T(".ts"))) {
        ctx.warn(_T("一般ファイルモードでのTSファイルの処理は非推奨です"));
    }

    // 一時ファイルのパスを使うので、transcodeMain と同じく最初に一時ディレクトリを作る
    const_cast<ConfigWrapper&>(setting).CreateTempDir();

    auto encoder = std::unique_ptr<AMTSimpleVideoEncoder>(new AMTSimpleVideoEncoder(ctx, setting));
    encoder->encode();
    int audioCount = encoder->getAudioCount();
    int64_t srcFileSize = encoder->getSrcFileSize();
    VideoFormat videoFormat = encoder->getVideoFormat();
    encoder = nullptr;

    auto muxer = std::unique_ptr<AMTSimpleMuxder>(new AMTSimpleMuxder(ctx, setting));
    muxer->mux(videoFormat, audioCount);
    int64_t totalOutSize = muxer->getTotalOutSize();
    muxer = nullptr;

    // 出力結果を表示
    ctx.info(_T("完了"));
    if (setting.getOutInfoJsonPath().size() > 0) {
        StringBuilder sb;
        sb.append("{ \"srcpath\": \"%s\"", toJsonString(setting.getSrcFilePath()))
            .append(", \"outpath\": \"%s\"", toJsonString(setting.getOutFilePath(EncodeFileKey(), EncodeFileKey(), setting.getFormat(), videoFormat.format)))
            .append(", \"srcfilesize\": %lld", srcFileSize)
            .append(", \"outfilesize\": %lld", totalOutSize)
            .append(" }");

        std::string str = sb.str();
        MemoryChunk mc(reinterpret_cast<uint8_t*>(const_cast<char*>(str.data())), str.size());
        File file(setting.getOutInfoJsonPath(), _T("w"));
        file.write(mc);
    }
}
DrcsSearchSplitter::DrcsSearchSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, false, true)
    , setting_(setting) {}

void DrcsSearchSplitter::readAll() {
    enum { BUFSIZE = 4 * 1024 * 1024 };
    auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
    MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
    File srcfile(setting_.getSrcFilePath(), _T("rb"));
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        inputTsData(MemoryChunk(buffer.data, readBytes));
    } while (readBytes == buffer.length);
}

// TsSplitter仮想関数 //

/* virtual */ void DrcsSearchSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // 今の所最初のフレームしか必要ないけど
    for (const VideoFrameInfo& frame : frames) {
        videoFrameList_.push_back(frame);
    }
}

/* virtual */ void DrcsSearchSplitter::onVideoFormatChanged(VideoFormat fmt) {}

/* virtual */ void DrcsSearchSplitter::onAudioPesPacket(
    int audioIdx,
    int64_t clock,
    const std::vector<AudioFrameData>& frames,
    PESPacket packet, const AudioComponentInfo& component) {}

/* virtual */ void DrcsSearchSplitter::onAudioFormatChanged(int audioIdx, AudioFormat fmt) {}

/* virtual */ void DrcsSearchSplitter::onCaptionPesPacket(
    int64_t clock,
    std::vector<CaptionItem>& captions,
    PESPacket packet) {}

/* virtual */ DRCSOutInfo DrcsSearchSplitter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    DRCSOutInfo info;
    info.elapsed = (videoFrameList_.size() > 0) ? (double)(PTS - videoFrameList_[0].PTS) : -1.0;
    info.filename = setting_.getDRCSOutPath(md5);
    return info;
}

/* virtual */ void DrcsSearchSplitter::onTime(int64_t clock, JSTTime time) {}
SubtitleDetectorSplitter::SubtitleDetectorSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, false, true)
    , setting_(setting)
    , hasSubtltle_(false) {}

void SubtitleDetectorSplitter::readAll(int maxframes) {
    enum { BUFSIZE = 4 * 1024 * 1024 };
    auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
    MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
    File srcfile(setting_.getSrcFilePath(), _T("rb"));
    auto fileSize = srcfile.size();
    // ファイル先頭から10%のところから読む
    srcfile.seek(fileSize / 10, SEEK_SET);
    int64_t totalRead = 0;
    // 最後の10%は読まない
    int64_t end = fileSize / 10 * 9;
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        inputTsData(MemoryChunk(buffer.data, readBytes));
        totalRead += readBytes;
    } while (totalRead < end && !hasSubtltle_ && (int)videoFrameList_.size() < maxframes);
}

bool SubtitleDetectorSplitter::getHasSubtitle() const {
    return hasSubtltle_;
}

// TsSplitter仮想関数 //

/* virtual */ void SubtitleDetectorSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // 今の所最初のフレームしか必要ないけど
    for (const VideoFrameInfo& frame : frames) {
        videoFrameList_.push_back(frame);
    }
}

/* virtual */ void SubtitleDetectorSplitter::onVideoFormatChanged(VideoFormat fmt) {}

/* virtual */ void SubtitleDetectorSplitter::onAudioPesPacket(
    int audioIdx,
    int64_t clock,
    const std::vector<AudioFrameData>& frames,
    PESPacket packet, const AudioComponentInfo& component) {}

/* virtual */ void SubtitleDetectorSplitter::onAudioFormatChanged(int audioIdx, AudioFormat fmt) {}

/* virtual */ void SubtitleDetectorSplitter::onCaptionPesPacket(
    int64_t clock,
    std::vector<CaptionItem>& captions,
    PESPacket packet) {}

/* virtual */ DRCSOutInfo SubtitleDetectorSplitter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    return DRCSOutInfo();
}

/* virtual */ void SubtitleDetectorSplitter::onTime(int64_t clock, JSTTime time) {}

/* virtual */ void SubtitleDetectorSplitter::onCaptionPacket(int64_t clock, TsPacket packet) {
    hasSubtltle_ = true;
}
AudioDetectorSplitter::AudioDetectorSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, true, false)
    , setting_(setting) {}

void AudioDetectorSplitter::readAll(int maxframes) {
    enum { BUFSIZE = 4 * 1024 * 1024 };
    auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
    MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
    File srcfile(setting_.getSrcFilePath(), _T("rb"));
    auto fileSize = srcfile.size();
    // ファイル先頭から10%のところから読む
    srcfile.seek(fileSize / 10, SEEK_SET);
    int64_t totalRead = 0;
    // 最後の10%は読まない
    int64_t end = fileSize / 10 * 9;
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        inputTsData(MemoryChunk(buffer.data, readBytes));
        totalRead += readBytes;
    } while (totalRead < end && (int)videoFrameList_.size() < maxframes);
}

// TsSplitter仮想関数 //

/* virtual */ void AudioDetectorSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // 今の所最初のフレームしか必要ないけど
    for (const VideoFrameInfo& frame : frames) {
        videoFrameList_.push_back(frame);
    }
}

/* virtual */ void AudioDetectorSplitter::onVideoFormatChanged(VideoFormat fmt) {}

/* virtual */ void AudioDetectorSplitter::onAudioPesPacket(
    int audioIdx,
    int64_t clock,
    const std::vector<AudioFrameData>& frames,
    PESPacket packet, const AudioComponentInfo& component) {}

/* virtual */ void AudioDetectorSplitter::onAudioFormatChanged(int audioIdx, AudioFormat fmt) {
    _ftprintf(stdout, _T("インデックス: %d チャンネル: %s サンプルレート: %d\n"),
        audioIdx, getAudioChannelString(fmt.channels), fmt.sampleRate);
}

/* virtual */ void AudioDetectorSplitter::onCaptionPesPacket(
    int64_t clock,
    std::vector<CaptionItem>& captions,
    PESPacket packet) {}

/* virtual */ DRCSOutInfo AudioDetectorSplitter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    return DRCSOutInfo();
}

/* virtual */ void AudioDetectorSplitter::onTime(int64_t clock, JSTTime time) {}

/* static */ void searchDrcsMain(AMTContext& ctx, const ConfigWrapper& setting) {
    Stopwatch sw;
    sw.start();
    auto splitter = std::unique_ptr<DrcsSearchSplitter>(new DrcsSearchSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    splitter->readAll();
    ctx.infoF(_T("完了: %.2f秒"), sw.getAndReset());
}

/* static */ void detectSubtitleMain(AMTContext& ctx, const ConfigWrapper& setting) {
    auto splitter = std::unique_ptr<SubtitleDetectorSplitter>(new SubtitleDetectorSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    splitter->readAll(setting.getMaxFrames());
    printf("字幕%s\n", splitter->getHasSubtitle() ? "あり" : "なし");
}

/* static */ void detectAudioMain(AMTContext& ctx, const ConfigWrapper& setting) {
    auto splitter = std::unique_ptr<AudioDetectorSplitter>(new AudioDetectorSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    splitter->readAll(setting.getMaxFrames());
}

/* --------------------------------------------------------------------
EXTREME TUXRACER -- Android asset extraction

The game reads its 52 MB of data through ordinary fopen() paths, in
hundreds of places (CSPList, course loading, texture lists, ...). Teaching
all of that to speak AAssetManager would be invasive and error-prone, so
instead the whole data tree is unpacked once from the APK into internal
storage and param.data_dir is pointed at it. After that every existing
path in the game just works.

Rather than ship a zip and link a decompressor, the build writes a plain
manifest listing every asset path. Extraction walks that list, which keeps
this dependency-free and makes a partial unpack easy to detect and repeat.
---------------------------------------------------------------------*/

#include "platform.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "ETRAssets", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRAssets", __VA_ARGS__)

namespace etr_platform {

namespace {

std::string g_internal_path;

/** Bumped by the build whenever the data payload changes, so an upgraded
 *  APK re-extracts instead of running against a stale tree. */
const char* kManifestName = "data_manifest.txt";
const char* kStampName    = "data_stamp.txt";

bool makeDirs(const std::string& path) {
	// Creates every missing component of a directory path.
	std::string acc;
	for (std::size_t i = 0; i < path.size(); i++) {
		acc.push_back(path[i]);
		if (path[i] == '/' || i + 1 == path.size()) {
			if (acc == "/" || acc.empty()) continue;
			std::string dir = acc;
			if (dir.back() == '/') dir.pop_back();
			if (mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) {
				LOGE("mkdir('%s') failed: %s", dir.c_str(), std::strerror(errno));
				return false;
			}
		}
	}
	return true;
}

AAssetManager* assetManager() {
	android_app* app = GetAndroidApp();
	return app ? app->activity->assetManager : nullptr;
}

/** Reads a whole file out of the APK's assets. */
bool readAsset(const std::string& name, std::vector<char>& out) {
	AAssetManager* mgr = assetManager();
	if (!mgr) return false;

	AAsset* asset = AAssetManager_open(mgr, name.c_str(), AASSET_MODE_BUFFER);
	if (!asset) return false;

	const off64_t size = AAsset_getLength64(asset);
	out.resize(static_cast<std::size_t>(size));

	const int got = AAsset_read(asset, out.data(),
	                            static_cast<size_t>(size));
	AAsset_close(asset);

	if (got < 0 || static_cast<std::size_t>(got) != out.size()) {
		LOGE("short read on asset '%s' (%d of %zu)", name.c_str(), got,
		     out.size());
		return false;
	}
	return true;
}

bool copyAssetToFile(const std::string& asset, const std::string& dest) {
	AAssetManager* mgr = assetManager();
	if (!mgr) return false;

	AAsset* in = AAssetManager_open(mgr, asset.c_str(), AASSET_MODE_STREAMING);
	if (!in) {
		LOGE("missing asset '%s'", asset.c_str());
		return false;
	}

	FILE* f = std::fopen(dest.c_str(), "wb");
	if (!f) {
		LOGE("cannot write '%s': %s", dest.c_str(), std::strerror(errno));
		AAsset_close(in);
		return false;
	}

	char buffer[64 * 1024];
	bool ok = true;
	for (;;) {
		const int got = AAsset_read(in, buffer, sizeof(buffer));
		if (got == 0) break;
		if (got < 0) {
			LOGE("read failed for asset '%s'", asset.c_str());
			ok = false;
			break;
		}
		if (std::fwrite(buffer, 1, static_cast<std::size_t>(got), f) !=
		    static_cast<std::size_t>(got)) {
			LOGE("write failed for '%s'", dest.c_str());
			ok = false;
			break;
		}
	}

	std::fclose(f);
	AAsset_close(in);
	return ok;
}

std::string readTextFile(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return std::string();

	std::string out;
	char buf[256];
	std::size_t got;
	while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, got);

	std::fclose(f);
	return out;
}

}  // namespace

const std::string& GetInternalDataPath() {
	if (g_internal_path.empty()) {
		android_app* app = GetAndroidApp();
		const char* path =
		    (app && app->activity) ? app->activity->internalDataPath : nullptr;
		g_internal_path = path ? path : ".";
		if (!g_internal_path.empty() && g_internal_path.back() == '/')
			g_internal_path.pop_back();
	}
	return g_internal_path;
}

bool ExtractGameData(void (*progress)(float)) {
	const std::string root = GetInternalDataPath();

	std::vector<char> manifest_data;
	if (!readAsset(kManifestName, manifest_data)) {
		LOGE("no '%s' in the APK -- was the data payload staged?",
		     kManifestName);
		return false;
	}

	// The first line is the payload version; the rest are relative paths.
	std::string manifest(manifest_data.begin(), manifest_data.end());
	std::size_t nl = manifest.find('\n');
	if (nl == std::string::npos) {
		LOGE("malformed manifest");
		return false;
	}

	std::string version = manifest.substr(0, nl);
	while (!version.empty() && (version.back() == '\r' || version.back() == ' '))
		version.pop_back();

	const std::string stamp_path = root + "/" + kStampName;
	if (readTextFile(stamp_path) == version) {
		LOGI("data already extracted (version %s)", version.c_str());
		if (progress) progress(1.f);
		return true;
	}

	LOGI("extracting game data, version %s", version.c_str());

	// Collect the paths first so progress can be reported meaningfully.
	std::vector<std::string> entries;
	std::size_t pos = nl + 1;
	while (pos < manifest.size()) {
		std::size_t end = manifest.find('\n', pos);
		if (end == std::string::npos) end = manifest.size();

		std::string line = manifest.substr(pos, end - pos);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		if (!line.empty()) entries.push_back(line);

		pos = end + 1;
	}

	if (entries.empty()) {
		LOGE("manifest lists no files");
		return false;
	}

	for (std::size_t i = 0; i < entries.size(); i++) {
		const std::string& rel = entries[i];
		const std::string dest = root + "/" + rel;

		const std::size_t slash = dest.rfind('/');
		if (slash != std::string::npos && !makeDirs(dest.substr(0, slash + 1)))
			return false;

		if (!copyAssetToFile(rel, dest)) return false;

		if (progress && (i % 16 == 0)) {
			progress(static_cast<float>(i + 1) /
			         static_cast<float>(entries.size()));
		}
	}

	// The stamp is written last, so an interrupted extraction is retried
	// rather than being mistaken for a complete one.
	FILE* f = std::fopen(stamp_path.c_str(), "wb");
	if (!f) {
		LOGE("cannot write the version stamp");
		return false;
	}
	std::fwrite(version.data(), 1, version.size(), f);
	std::fclose(f);

	LOGI("extracted %zu files", entries.size());
	if (progress) progress(1.f);
	return true;
}

}  // namespace etr_platform

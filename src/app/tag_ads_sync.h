#pragma once
#include "places.h"
namespace pulse::app {
// Called on the serial ADS lane; persists intent before changing any stream.
std::vector<std::wstring> SyncTagAdsUpdates(const std::vector<TagAdsUpdate>& updates);
bool HasPendingTagAds(const std::wstring& path);
}

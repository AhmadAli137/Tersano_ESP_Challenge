#pragma once

/*
 * Safe template for local secrets.
 *
 * Setup:
 * 1) Copy this file to: include/secrets.h
 * 2) Replace placeholder values with your real credentials
 * 3) Keep secrets.h out of source control (.gitignore)
 */

namespace secrets {

// Wi-Fi credentials used by the device to join your network.
static constexpr const char* WIFI_SSID = "YOUR_WIFI_SSID";
static constexpr const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Supabase project endpoint and publishable (anon/public) API key.
// Never place service-role keys in firmware.
static constexpr const char* SUPABASE_URL = "https://YOUR_PROJECT_ID.supabase.co";
static constexpr const char* SUPABASE_API_KEY = "YOUR_SUPABASE_PUBLISHABLE_KEY";

}  // namespace secrets

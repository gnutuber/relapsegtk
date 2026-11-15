#pragma once

#include <gio/gio.h>

#include "account.h"

char *profile_cache_directory(void);
gboolean profile_cache_lookup(const char *cookie, gboolean *valid, ProfileAccount **account);
gboolean profile_cache_store(const ProfileAccount *account);
gboolean profile_cache_store_invalid(const char *cookie);
gboolean profile_cache_claim(const char *cookie);
void profile_cache_release(const char *cookie);

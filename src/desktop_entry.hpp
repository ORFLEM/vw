#pragma once

// Desktop entry lookup shared inside vw (notifications use an app's icon).

#include "vw/applications.hpp"

#include <optional>
#include <string_view>

namespace vw::applications {

// The installed entry with this desktop file id ("firefox.desktop"),
// including ones hidden from menus (NoDisplay). Null when there is none.
std::optional<Application> find(std::string_view id);

}  // namespace vw::applications

# vw as a nixpkgs package. The flake passes `src` and `zwwm`; through
# overlays.default it lands in pkgs as `pkgs.vw`.
{
  lib,
  clangStdenv,
  src,
  zwwm,
  # Facet: vw-facetc, <vw/facet.hpp> and facet_compile() (VW_WITH_FACET).
  withFacet ? false,
  cmake,
  ninja,
  pkg-config,
  shaderc,
  wayland-scanner,
  cairo,
  fontconfig,
  freetype,
  harfbuzz,
  libxkbcommon,
  libxml2,
  lua5_4,
  nlohmann_json,
  pam,
  pango,
  pipewire,
  quickjs,
  libsysprof-capture,
  vulkan-headers,
  vulkan-loader,
  wayland,
  wayland-protocols,
}:

clangStdenv.mkDerivation {
  pname = "vw";
  version = "0.1.0";
  inherit src;

  nativeBuildInputs = [ cmake ninja pkg-config shaderc wayland-scanner ];
  buildInputs = [
    # libxml2: zwwm's zwayland-scanner, built with vw, reads protocol XML with it.
    cairo fontconfig freetype harfbuzz libxkbcommon libxml2 lua5_4
    pam pango pipewire quickjs libsysprof-capture vulkan-headers vulkan-loader wayland
    wayland-protocols
  ];
  # vw's public headers include nlohmann/json.hpp, and vwConfig.cmake finds it.
  propagatedBuildInputs = [ nlohmann_json ];

  cmakeFlags = [ "-DZWAYLAND_ROOT=${zwwm}" (lib.cmakeBool "VW_WITH_FACET" withFacet) ];

  meta = {
    description = "C++23 Wayland widget library with its own Vulkan renderer";
    homepage = "https://vw.zapzig.org";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
    mainProgram = "vw";
  };
}

{
  description = "Diorama64 Quest 3S port dev shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    openxr-sdk = {
      url = "github:KhronosGroup/OpenXR-SDK/release-1.1.65";
      flake = false;
    };
    glslang = {
      url = "github:KhronosGroup/glslang/15.4.0";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, openxr-sdk, glslang }:
    let
      pkgs = import nixpkgs {
        system = "x86_64-linux";
        config = {
          allowUnfree = true;
          android_sdk.accept_license = true;
        };
      };
      android = pkgs.androidenv.composeAndroidPackages {
        includeNDK = true;
        platformVersions = [ "34" ];
        buildToolsVersions = [ "34.0.0" ];
        includeEmulator = false;
        includeSystemImages = false;
      };
    in
    {
      devShells.x86_64-linux.default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
        packages = with pkgs; [
          cmake ninja gnumake pkg-config python3 jdk17 imagemagick libarchive
          SDL2 glew zlib vulkan-headers vulkan-loader vulkan-validation-layers
          vulkan-tools mesa git
          monado libpulseaudio   # desktop OpenXR testing without a headset (scripts/desktop_xr.py)
          android.androidsdk
        ];
        shellHook = ''
          export ANDROID_SDK_ROOT="${android.androidsdk}/libexec/android-sdk"
          if [ -d "$ANDROID_SDK_ROOT/ndk-bundle" ]; then
            export ANDROID_NDK_ROOT="$ANDROID_SDK_ROOT/ndk-bundle"
          else
            export ANDROID_NDK_ROOT="$(ls -d "$ANDROID_SDK_ROOT"/ndk/* | head -n1)"
          fi
          export ANDROID_BUILD_TOOLS="$(dirname "$(ls "$ANDROID_SDK_ROOT"/build-tools/*/aapt2 | head -n1)")"
          export ANDROID_JAR="$ANDROID_SDK_ROOT/platforms/android-34/android.jar"
          export JAVA_HOME="${pkgs.jdk17.home}"
          export SM64AR_OPENXR_DIR="${openxr-sdk}"
          export SM64AR_GLSLANG_DIR="${glslang}"
          export VK_DRIVER_FILES="${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.x86_64.json"
          export SM64AR_MONADO_JSON="${pkgs.monado}/share/openxr/1/openxr_monado.json"
        '';
      };
    };
}

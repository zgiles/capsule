{ lib, stdenv, libcap, src }:
stdenv.mkDerivation {
  pname   = "capsule";
  version = "1.0.0";

  inherit src;

  buildInputs = [ libcap ];

  buildPhase = "make capsule";

  installPhase = ''
    install -D -m 0755 capsule                    $out/bin/capsule
    install -D -m 0755 scripts/capsule-netns-up   $out/bin/capsule-netns-up
    install -D -m 0755 scripts/capsule-netns-down $out/bin/capsule-netns-down
    install -D -m 0644 capsule.1                  $out/share/man/man1/capsule.1
  '';

  meta = with lib; {
    description = "Capability-based network namespace executor for Linux";
    homepage    = "https://github.com/zgiles/capsule";
    license     = licenses.mit;
    platforms   = [ "x86_64-linux" "aarch64-linux" ];
    mainProgram = "capsule";
  };
}

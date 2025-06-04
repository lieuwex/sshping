with (import (fetchTarball "https://github.com/NixOS/nixpkgs/archive/release-24.05.tar.gz") {});

pkgs.mkShell {
#(mkShell.override { stdenv = clangStdenv; }) {
	buildInputs = [
		ccls
		bear
		gdb
		valgrind

		libssh
	];
}

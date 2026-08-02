{
  inputs = {
    nixpkgs = {
      url = "github:nixos/nixpkgs/nixos-unstable";
    };
    flake-utils = {
      url = "github:numtide/flake-utils";
    };
  };
  outputs = { nixpkgs, flake-utils, ... }: flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = import nixpkgs {
        inherit system;
      };
    in rec {
      devShell = pkgs.mkShell {
        buildInputs = with pkgs; [
	  python3
	  # Idk how to make vscode use these, so...
          #python3.withPackages(ps: with ps; [
          #  ipython
          #  jupyter
          #  matplotlib
          #  numpy
          #  pandas
          #]))
	  zsh
        ];
        shellHook = ''
          VENV_DIR="$(pwd)/venv"
          if ! [[ -d $VENV_DIR ]]; then
	    python -m venv --system-site-packages --copies $VENV_DIR
	    source $VENV_DIR/bin/activate
	    pip install -r requirements.pip
	  else
	    source $VENV_DIR/bin/activate
          fi
	'';
      };
    }
  );
}

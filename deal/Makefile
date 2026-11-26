.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.ONESHELL:

CFLAGS ?= -O2
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PACKAGES := wayland-client wayland-egl egl opengl xkbcommon freetype2 fontconfig
GENERATED := .build
XDG_HEADER := $(GENERATED)/xdg-shell-client-protocol.h
XDG_CODE := $(GENERATED)/xdg-shell-protocol.c
PRIMARY_HEADER := $(GENERATED)/primary-selection-unstable-v1-client-protocol.h
PRIMARY_CODE := $(GENERATED)/primary-selection-unstable-v1-protocol.c

# Clean and uninstall also work on systems without Wayland development files.
ifneq ($(filter-out clean uninstall,$(or $(MAKECMDGOALS),all)),)
PROTOCOL_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null)
ifndef XDG_SHELL_XML
XDG_SHELL_XML := $(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml
endif
PRIMARY_XML ?= $(PROTOCOL_DIR)/unstable/primary-selection/primary-selection-unstable-v1.xml
XDG_INPUT := $(wildcard $(XDG_SHELL_XML))
PRIMARY_INPUT := $(wildcard $(PRIMARY_XML))
endif

export DEAL_INSTALL_PREFIX := $(or $(value PREFIX),$(if $(value HOME),$(value HOME)/.local))
export DEAL_INSTALL_DESTDIR := $(value DESTDIR)

.PHONY: all check-build install uninstall clean

all: deal

deal: terminal.c terminal.h $(XDG_HEADER) $(XDG_CODE) $(PRIMARY_HEADER) $(PRIMARY_CODE) Makefile | check-build
	set -eu
	native_cflags=$$($(PKG_CONFIG) --cflags $(PACKAGES))
	native_libs=$$($(PKG_CONFIG) --libs $(PACKAGES))
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(CFLAGS) \
	    $$native_cflags -I"$(GENERATED)" $(LDFLAGS) -o $@ terminal.c "$(XDG_CODE)" \
	    "$(PRIMARY_CODE)" -lutil $$native_libs $(LDLIBS)

check-build:
	set -eu
	for tool in "$(firstword $(PKG_CONFIG))" "$(firstword $(WAYLAND_SCANNER))" "$(firstword $(CC))"; do
	    if ! command -v "$$tool" >/dev/null 2>&1; then
	        printf 'deal: required build tool missing: %s\n' "$$tool" >&2
	        exit 1
	    fi
	done
	if ! $(PKG_CONFIG) --print-errors --exists $(PACKAGES) wayland-protocols; then
	    printf '%s\n' 'deal: required development files are missing (see README.md for Arch Linux packages)' >&2
	    exit 1
	fi
	if [ ! -r "$(XDG_SHELL_XML)" ]; then
	    printf 'deal: cannot read xdg-shell protocol XML at %s\n' "$(XDG_SHELL_XML)" >&2
	    exit 1
	fi
	if [ ! -r "$(PRIMARY_XML)" ]; then
	    printf 'deal: cannot read primary-selection protocol XML at %s\n' "$(PRIMARY_XML)" >&2
	    exit 1
	fi

$(GENERATED):
	mkdir -p -- "$@"

$(XDG_HEADER): $(XDG_INPUT) | check-build $(GENERATED)
	$(WAYLAND_SCANNER) client-header "$(XDG_SHELL_XML)" "$@"

$(XDG_CODE): $(XDG_INPUT) | check-build $(GENERATED)
	$(WAYLAND_SCANNER) private-code "$(XDG_SHELL_XML)" "$@"

$(PRIMARY_HEADER): $(PRIMARY_INPUT) | check-build $(GENERATED)
	$(WAYLAND_SCANNER) client-header "$(PRIMARY_XML)" "$@"

$(PRIMARY_CODE): $(PRIMARY_INPUT) | check-build $(GENERATED)
	$(WAYLAND_SCANNER) private-code "$(PRIMARY_XML)" "$@"

clean:
	rm -f -- deal "$(XDG_HEADER)" "$(XDG_CODE)" "$(PRIMARY_HEADER)" "$(PRIMARY_CODE)"
	rmdir -- "$(GENERATED)" 2>/dev/null || true

install: deal deal.conf

install uninstall:
	set -eu
	prefix=$${DEAL_INSTALL_PREFIX:?HOME or PREFIX is required}
	dest=$$DEAL_INSTALL_DESTDIR
	case "$$prefix" in /*) ;; *) printf '%s\n' 'deal: PREFIX must be absolute' >&2; exit 1 ;; esac
	case "$$dest" in ''|/*) ;; *) printf '%s\n' 'deal: DESTDIR must be absolute' >&2; exit 1 ;; esac
	case "$$prefix" in
	    *'
	'*) printf '%s\n' 'deal: PREFIX must fit on one line' >&2; exit 1 ;;
	esac
	if printf '%s' "$$prefix" | LC_ALL=C grep -q '[[:cntrl:]=]'; then
	    printf '%s\n' 'deal: PREFIX contains a character unsupported in desktop launchers' >&2
	    exit 1
	fi
	root=$$dest$$prefix
	manifest=$$root/share/deal/.install-manifest
	paths='bin/deal share/applications/deal.desktop share/xfce4/helpers/deal.desktop share/deal/deal.conf share/deal/deal.example.conf share/deal/init.example.lua share/deal/deal.terminfo share/terminfo/d/deal share/terminfo/64/deal'

	checksum() {
	    sha256sum < "$$1" | cut -d ' ' -f 1
	}

	owned() {
	    [ -f "$$manifest" ] && [ ! -L "$$manifest" ] &&
	        [ -f "$$root/$$1" ] && [ ! -L "$$root/$$1" ] &&
	        grep -Fxq "$$(checksum "$$root/$$1") $$1" "$$manifest"
	}

	case "$@" in
	    uninstall)
	        if [ ! -f "$$manifest" ] || [ -L "$$manifest" ] ||
	            ! grep -Fxq 'deal installation v1' "$$manifest"; then
	            printf '%s\n' "deal: no installation recorded under $$root" >&2
	            exit 1
	        fi
	        for relative in $$paths; do
	            if owned "$$relative"; then
	                rm -- "$$root/$$relative"
	            elif [ -e "$$root/$$relative" ] || [ -L "$$root/$$relative" ]; then
	                printf '%s\n' "deal: kept modified or untracked file $$root/$$relative" >&2
	            fi
	        done
	        rm -- "$$manifest"
	        rmdir -- "$$root/share/deal" 2>/dev/null || true
	        exit 0 ;;
	    install) ;;
	esac

	stage=$$(mktemp -d)
	trap 'rm -rf -- "$$stage"' EXIT
	trap 'exit 1' HUP INT TERM
	install -Dm755 deal "$$stage/bin/deal"
	install -Dm644 deal.conf "$$stage/share/deal/deal.conf"
	printf '%s\n' "$$DEAL_TERMINFO" > "$$stage/share/deal/deal.terminfo"
	mkdir -p -- "$$stage/share/applications" "$$stage/share/xfce4/helpers"
	executable=$$prefix/bin/deal
	# Desktop Entry string escaping precedes Exec quoting.
	exec_path=$$(printf '%s' "$$executable" | sed -e 's/\\/\\\\\\\\/g' -e 's/["`$$]/\\\\&/g' -e 's/%/%%/g')
	try_path=$$(printf '%s' "$$executable" | sed 's/\\/\\\\/g')
	{
	    cat <<'DESKTOP'
	[Desktop Entry]
	Type=Application
	Name=deal
	GenericName=Terminal Emulator
	Comment=Run a shell or terminal applications
	Icon=utilities-terminal
	Terminal=false
	Categories=System;TerminalEmulator;
	Keywords=shell;console;terminal;
	DESKTOP
	    printf 'Exec="%s"\nTryExec=%s\n' "$$exec_path" "$$try_path"
	} > "$$stage/share/applications/deal.desktop"
	{
	    cat <<'XFCE'
	[Desktop Entry]
	Type=X-XFCE-Helper
	Name=deal
	Icon=utilities-terminal
	X-XFCE-Category=TerminalEmulator
	X-XFCE-Commands=%B;
	X-XFCE-CommandsWithParameter=%B -e %s;
	XFCE
	    printf 'X-XFCE-Binaries=%s;\n' "$$(printf '%s' "$$try_path" | sed 's/;/\\;/g')"
	} > "$$stage/share/xfce4/helpers/deal.desktop"
	if command -v tic >/dev/null 2>&1; then
	    mkdir -p -- "$$stage/share/terminfo"
	    tic -x -o "$$stage/share/terminfo" "$$stage/share/deal/deal.terminfo"
	else
	    printf '%s\n' 'deal: tic unavailable, installed terminfo source. xterm-color remains available.' >&2
	fi

	if [ -e "$$manifest" ] || [ -L "$$manifest" ]; then
	    if [ -L "$$manifest" ] || ! grep -Fxq 'deal installation v1' "$$manifest"; then
	        printf '%s\n' "deal: refusing to replace $$manifest" >&2
	        exit 1
	    fi
	fi
	for relative in $$paths; do
	    if [ -f "$$stage/$$relative" ] && { [ -e "$$root/$$relative" ] || [ -L "$$root/$$relative" ]; } &&
	        ! owned "$$relative"; then
	        printf '%s\n' "deal: refusing to replace $$root/$$relative" >&2
	        exit 1
	    fi
	done
	{
	    printf '%s\n' 'deal installation v1'
	    for relative in $$paths; do
	        if [ -f "$$stage/$$relative" ]; then
	            case "$$relative" in bin/*) mode=755 ;; *) mode=644 ;; esac
	            install -Dm"$$mode" -- "$$stage/$$relative" "$$root/$$relative"
	            printf '%s %s\n' "$$(checksum "$$root/$$relative")" "$$relative"
	        elif owned "$$relative"; then
	            case "$$relative" in
	                share/deal/init.example.lua|share/deal/deal.example.conf)
	                    rm -- "$$root/$$relative" ;;
	                *) printf '%s %s\n' "$$(checksum "$$root/$$relative")" "$$relative" ;;
	            esac
	        fi
	    done
	} > "$$stage/manifest"
	install -Dm644 -- "$$stage/manifest" "$$manifest"
	printf '%s\n' "Installed deal under $$root"

# Keep the indentation of terminfo continuation lines when writing the source.
define DEAL_TERMINFO
# Optional local entry. xterm-color is deal's default for portability.
deal|deal terminal emulator,
	am, bce, km, mir, msgr, xenl,
	cols#80, lines#24, it#8, colors#256, pairs#32767,
	bel=^G, cr=^M, ht=^I, hts=\EH, tbc=\E[3g,
	clear=\E[H\E[2J, ed=\E[J, el=\E[K, el1=\E[1K, E3=\E[3J,
	cup=\E[%i%p1%d;%p2%dH, home=\E[H, hpa=\E[%i%p1%dG, vpa=\E[%i%p1%dd,
	cub1=^H, cuf1=\E[C, cuu1=\E[A, cud1=\E[B,
	cub=\E[%p1%dD, cuf=\E[%p1%dC, cuu=\E[%p1%dA, cud=\E[%p1%dB,
	csr=\E[%i%p1%d;%p2%dr, ind=\ED, ri=\EM,
	il1=\E[L, dl1=\E[M, ich1=\E[@, dch1=\E[P, ech=\E[%p1%dX,
	il=\E[%p1%dL, dl=\E[%p1%dM, ich=\E[%p1%d@, dch=\E[%p1%dP,
	sc=\E7, rc=\E8, civis=\E[?25l, cnorm=\E[?25h,
	smcup=\E[?1049h, rmcup=\E[?1049l, smam=\E[?7h, rmam=\E[?7l,
	smir=\E[4h, rmir=\E[4l, rs1=\Ec, rs2=\E[!p,
	acsc=__``aaffggiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz{{||}}~~,
	smacs=\E(0, rmacs=\E(B,
	sgr0=\E[0m\E(B, bold=\E[1m, dim=\E[2m, rev=\E[7m,
	smso=\E[7m, rmso=\E[27m, smul=\E[4m, rmul=\E[24m, op=\E[39;49m,
	setaf=\E[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m,
	setab=\E[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d%e48;5;%p1%d%;m,
	smkx=\E[?1h\E=, rmkx=\E[?1l\E>,
	kbs=^?, kent=\EOM, kcbt=\E[Z,
	kcuu1=\EOA, kcud1=\EOB, kcuf1=\EOC, kcub1=\EOD,
	khome=\E[1~, kend=\E[4~, kich1=\E[2~, kdch1=\E[3~, kpp=\E[5~, knp=\E[6~,
	kf1=\E[11~, kf2=\E[12~, kf3=\E[13~, kf4=\E[14~,
	kf5=\E[15~, kf6=\E[17~, kf7=\E[18~, kf8=\E[19~,
	kf9=\E[20~, kf10=\E[21~, kf11=\E[23~, kf12=\E[24~,
	kmous=\E[<, XM=\E[?1006;1000%?%p1%{1}%=%th%el%;,
	BE=\E[?2004h, BD=\E[?2004l, PS=\E[200~, PE=\E[201~,
endef
export DEAL_TERMINFO

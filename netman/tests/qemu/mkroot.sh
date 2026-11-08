# mkroot.sh - generic host-side helpers to assemble a tiny initramfs root in $ROOT (sourced by run). Reusable by other projects.
qt_root_init() {
	mkdir -p "$ROOT"/{usr/bin,usr/lib,dev,proc,sys,tmp,etc,repo,out,root,run,var,qt/mods}
	ln -s usr/bin "$ROOT/bin"; ln -s usr/bin "$ROOT/sbin"; ln -s usr/lib "$ROOT/lib"; ln -s usr/lib "$ROOT/lib64"
	declare -gA QT_SEEN=()
}
qt_add_libs() { # copy every shared lib of an ELF (by ldd) to the same resolved dir inside $ROOT
	local l p d
	while read -r l; do
		p=$(awk '{ for (i=1;i<=NF;i++) if ($i ~ /^\//) { print $i; exit } }' <<<"$l"); [ -n "$p" ] && [ -e "$p" ] || continue
		d=$(readlink -f "$(dirname "$p")"); [ -e "$ROOT$d/$(basename "$p")" ] && continue
		mkdir -p "$ROOT$d"; cp -L "$p" "$ROOT$d/$(basename "$p")"
	done < <(ldd "$1" 2>/dev/null)
}
qt_add_bin() { # host binary -> /usr/bin (+ its libs)
	local p b; p=$(readlink -f "$1"); b=$(basename "$1")
	[ -n "${QT_SEEN[bin:$b]:-}" ] && return 0; QT_SEEN[bin:$b]=1
	cp -L "$p" "$ROOT/usr/bin/$b"; qt_add_libs "$p"
}
qt_add_file() { [ -e "$1" ] && { mkdir -p "$ROOT$(dirname "$1")"; cp -L "$1" "$ROOT$1"; }; }
qt_add_mod() { # module name -> /qt/mods/<name>.list (insmod order, deps included); .ko.zst decompressed; absent module = no list (stages SKIP)
	local m=$1 line f n out="$ROOT/qt/mods/$1.list"
	local deps; deps=$(modprobe -S "$(uname -r)" --show-depends "$m" 2>/dev/null) || return 0
	[ -n "$deps" ] || return 0
	: > "$out"
	while read -r kw f _; do
		[ "$kw" = insmod ] || continue
		n=$(basename "$f" .zst); n=${n%.ko}
		if [ -z "${QT_SEEN[mod:$n]:-}" ]; then QT_SEEN[mod:$n]=1; case $f in *.zst) zstd -dq "$f" -o "$ROOT/qt/mods/$n.ko";; *.xz) xz -dc "$f" > "$ROOT/qt/mods/$n.ko";; *.gz) gzip -dc "$f" > "$ROOT/qt/mods/$n.ko";; *) cp "$f" "$ROOT/qt/mods/$n.ko";; esac; fi
		echo "$n" >> "$out"
	done <<<"$deps"
}

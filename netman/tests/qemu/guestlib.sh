# guestlib.sh - sourced by every stage inside the VM. Prints QTEST lines on the serial console and keeps full logs in /out (host scratch dir).
PATH=/usr/bin:/usr/lib/iwd; export PATH
have() { command -v "$1" >/dev/null 2>&1; }
pass() { echo "QTEST $1 PASS${2:+ $2}"; }
fail() { echo "QTEST $1 FAIL${2:+ $2}"; QT_FAILED=1; }
skip() { echo "QTEST $1 SKIP ${2:-}"; }
need() { # need stage bin... : SKIP + return 1 when a binary is absent
	local s=$1 b; shift; for b in "$@"; do have "$b" || { skip "$s" "missing binary: $b"; return 1; }; done; return 0
}
loadmod() { # loadmod name... : insmod with deps; 0 = loaded/builtin, 1 = module not available in this VM
	local m n; for m in "$@"; do
		[ -f "/qt/mods/$m.list" ] || return 1
		while read -r n; do grep -qx "$n" /qt/loaded 2>/dev/null && continue
			insmod "/qt/mods/$n.ko" 2>>/out/modload.log || { grep -q "File exists" /out/modload.log && true; }
			echo "$n" >> /qt/loaded; done < "/qt/mods/$m.list"
	done; return 0
}
run_logged() { # run_logged stage cmd... : PASS when exit 0; full output in /out/stage.log; last lines on the console on failure
	local s=$1; shift; timeout 60 "$@" > "/out/$s.log" 2>&1; local rc=$?
	if [ $rc = 0 ]; then pass "$s" "($(grep -ciE '^ *ok|pass' "/out/$s.log" 2>/dev/null) ok-lines)"; else fail "$s" "rc=$rc: $(tail -3 "/out/$s.log" | tr '\n' '|' | cut -c1-300)"; fi
	return $rc
}

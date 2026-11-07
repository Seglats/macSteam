#!/bin/sh
# MacVolvo tool for NotProton; written by MacVolvo.app, which also removes it.
# Steam runs this as a compatibility tool (toolmanifest.vdf: "/run %verb%"). It adds
# MacVolvo's Mac driver (a NotProton runner variant) and MacVolvo's mouse settings, then
# hands everything to NotProton's own tool, unchanged, so NotProton does the launch.
# The two names below are filled in by MacVolvo.app (NotProton.swift).
np_tool_name='@NP_TOOL@'
mv_variant='@VARIANT@'

np_tool="$HOME/Library/Application Support/Steam/compatibilitytools.d/$np_tool_name"
mv_settings="$HOME/Library/Application Support/MacVolvo NotProton/settings"

if [ ! -x "$np_tool/run" ]; then
  echo "MacVolvo: NotProton's tool $np_tool_name is missing. Open NotProton so it sets up Steam again, then open MacVolvo." >&2
  exit 1
fi

case "${1:-}" in
  run | waitforexitandrun)
    # The variant puts MacVolvo's winemac.so in a copy of NotProton's runner. A variant
    # chosen in the game's Compatibility page is kept; NotProton sorts and combines them.
    case "+${NOTPROTON_RUNNER_VARIANT:-}+" in
      *"+$mv_variant+"*) ;;
      *) NOTPROTON_RUNNER_VARIANT="${NOTPROTON_RUNNER_VARIANT:+$NOTPROTON_RUNNER_VARIANT+}$mv_variant"
         export NOTPROTON_RUNNER_VARIANT ;;
    esac
    # MacVolvo's mouse settings (Settings › General › Mouse). Only these names are read,
    # and a value the game's launch options already set wins.
    if [ -r "$mv_settings" ]; then
      while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
          WINEMAC_MOUSELOOK=* | WINEMAC_INPUT_VARIANT=* | WINEMAC_RAW_SOURCE=* | WINEMAC_MOUSELOOK_RAW_INTERVAL_US=*) ;;
          *) continue ;;
        esac
        case "${line#*=}" in *[!A-Za-z0-9._*!\;-]*) continue ;; esac
        key=${line%%=*}
        eval "[ -z \"\${$key+x}\" ]" || continue
        export "$line"
      done < "$mv_settings"
    fi
    ;;
esac

exec "$np_tool/run" "$@"

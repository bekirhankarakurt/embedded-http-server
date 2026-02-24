# Demo recording for the README. Regenerate with: vhs docs/demo.tape
# Requires a Release build in ./build, curl and jq.
Output docs/demo.gif

Set Shell "bash"
Set FontSize 14
Set Width 1400
Set Height 640
Set Padding 16
Set TypingSpeed 35ms
Set Theme "Dracula"

Hide
Type "export PS1='$ ' && cd build/examples && clear" Enter
Sleep 300ms
Show

Type "./ehs-panel --sim --port 8080 --auth admin:secret --config /tmp/device.json &" Enter
Sleep 1s
Enter
Sleep 300ms
Type "curl -s localhost:8080/api/status -o /dev/null -w '%{http_code}\n'   # auth required" Enter
Sleep 1s
Type "curl -s -u admin:secret localhost:8080/api/status | jq -c .telemetry" Enter
Sleep 1500ms
Type `curl -s -u admin:secret -X PATCH -H 'Content-Type: application/json' -d '{"hostname":"radar-node-7"}' localhost:8080/api/config | jq -c` Enter
Sleep 1500ms
Type `curl -s -u admin:secret -X PATCH -H 'Content-Type: application/json' -d '{"netmask":"255.0.255.0"}' localhost:8080/api/config | jq -c` Enter
Sleep 1500ms
Type "curl -s -u admin:secret localhost:8080/api/logs | jq -c '[.level, .msg]'" Enter
Sleep 2s
Type "kill -INT %1; wait" Enter
Sleep 1500ms
Hide
Type "rm -f /tmp/device.json" Enter

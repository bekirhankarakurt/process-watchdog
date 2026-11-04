# Records docs/demo.gif with charmbracelet/vhs:  vhs docs/demo.tape
Output docs/demo.gif

Set Shell "bash"
Set FontSize 15
Set Width 1100
Set Height 560
Set Padding 16
Set Framerate 12
Set TypingSpeed 35ms
Set Theme "Dracula"

Hide
Type "export PATH=$PWD/build:$PATH PW_CONTROL_SOCKET=/tmp/pwatchd-demo.sock PS1='$ ' && rm -rf /tmp/pwatchd-demo-logs && clear"
Enter
Show

Type "pwatchd --check -c docs/demo.ini"
Enter
Sleep 2.5s
Type "pwatchd -c docs/demo.ini 2> /tmp/pwatchd-demo.log &"
Enter
Sleep 1s
Type "clear; pwctl list"
Enter
Sleep 3s
Type "clear; pwctl list   # crash loop + frozen heartbeat"
Enter
Sleep 4s
Type "grep -E 'heartbeat|giving up|restarting' /tmp/pwatchd-demo.log | cut -c12-"
Enter
Sleep 4s
Type "clear; pwctl restart flaky-worker && pwctl status flaky-worker"
Enter
Sleep 3.5s
Type "pwctl shutdown"
Enter
Sleep 1.5s

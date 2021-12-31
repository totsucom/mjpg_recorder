# mjpg_recorder2.2
mjpg-streamerの出力するストリームをファイルに保存する

RaspberryPiOSやUbuntu上で動作確認を行っています。

保存されたmjpgファイルはffmpegなどで必要な形式に変換してください。

<u>使用例</u>
　自機で走るmjpg-streamerから60秒間録画し./test.mjpgファイルに書き出す<br>
　mjpg_recorder2.2 ./ test localhost -T 60 -N 1


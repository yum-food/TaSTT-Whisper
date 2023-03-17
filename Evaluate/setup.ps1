
cp ../x64/Debug/WhisperCLI.exe .
cp ../x64/Debug/Whisper.dll .

$MODEL_URL = "https://huggingface.co/datasets/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin"
$MODEL_FILE = $(Split-Path -Path $MODEL_URL -Leaf)
if (-Not (Test-Path $MODEL_FILE)) {
  echo "Fetch model"
  Invoke-WebRequest $MODEL_URL -OutFile $MODEL_FILE
}

$AUDIO_URL = "https://www.archive.org/download/usconstitution_1610_librivox/constitution_01_unitedstates_64kb.mp3"
#$AUDIO_URL = "https://www.archive.org/download/usconstitution_1610_librivox/constitution_02_unitedstates_128kb.mp3"
$AUDIO_FILE = $(Split-Path -Path $AUDIO_URL -Leaf)
if (-Not (Test-Path $AUDIO_FILE)) {
  echo "Fetch audio"
  Invoke-WebRequest $AUDIO_URL -OutFile $AUDIO_FILE
}

## Evaluation tool

This directory holds code to evaluate the accuracy of transcriptions.

Example usage (in Powershell):
```
python3 -m pip install -r requirements.txt
python3 evaluate.py reference_transcript.txt
./setup.ps1
./WhisperCLI.exe \
  --audio_path *.mp3 \
  --model_path *.bin
```

* setup.ps1: Downloads whisper checkpoints and audio from librivox. Copies
  WhisperCLI.exe to CWD.
* evaluate.py: Computes Levenshtein distance between generated transcription
  and "correct" transcription.

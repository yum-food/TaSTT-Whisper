import argparse
import editdistance
import re
import subprocess
import sys
import time

from whisper.normalizers import EnglishTextNormalizer

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("reference_path", type=str, help="Path to reference transcript")
    parser.add_argument("audio_path", type=str, help="Path to audio file to transcribe")
    parser.add_argument("model_path", type=str, help="Path to Whisper model to use")
    parser.add_argument("decode_method", type=str, help="Decoding method. Either 'greedy' or 'beam'")
    args = parser.parse_args()

    cmd = "./WhisperCLI.exe"
    cmd_args = [
            "--audio_path", args.audio_path,
            "--model_path", args.model_path,
            "--decode_method", args.decode_method,
            ]

    t0 = time.time()
    result = subprocess.run([cmd] + cmd_args, stdout=subprocess.PIPE)
    t1 = time.time()

    if result.returncode != 0:
        print(f"Failed to transcribe: cmd returned {result.returncode}",
                file=sys.stderr)

    test_transcript = result.stdout.decode("utf-8")
    with open(args.reference_path, "r") as f:
        ref_transcript = f.read()

    # Normalize transcripts before computing edit distance (as described in
    # whisper paper).
    normalize = EnglishTextNormalizer()
    test_transcript = normalize(test_transcript)
    ref_transcript = normalize(ref_transcript)

    dist = editdistance.eval(ref_transcript, test_transcript)

    print(f"Duration: {t1 - t0}")
    print(f"Levenshtein distance: {dist}")
    print(f"Transcript: {test_transcript}")


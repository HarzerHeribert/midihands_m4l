# Third-party components

The macOS release contains only MidiHands' own code. The Windows release also contains:

- **ONNX Runtime** 1.20.1 (`support/onnxruntime.dll`), © Microsoft Corporation, MIT License.
  https://github.com/microsoft/onnxruntime — license: https://github.com/microsoft/onnxruntime/blob/main/LICENSE
- **MediaPipe hand landmarker models** (`models/hand_detector.onnx`, `models/hand_landmarks.onnx`,
  converted to ONNX from Google's `hand_landmarker.task`), © Google LLC, Apache License 2.0.
  https://ai.google.dev/edge/mediapipe/solutions/vision/hand_landmarker — license:
  https://www.apache.org/licenses/LICENSE-2.0
- **stb_image_write** (compiled into the Windows external), by Sean Barrett, public domain / MIT.
  https://github.com/nothings/stb

The Max SDK headers used to build the externals are © Cycling '74 under the Max SDK license
(https://github.com/Cycling74/max-sdk-base).

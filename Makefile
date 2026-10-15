VENV ?= .venv
ifeq ($(OS),Windows_NT)
  PY ?= python
  BIN = $(VENV)/Scripts
  RM = powershell -NoProfile -Command Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
else
  # macOS / Ubuntu は python3 だけで python が無い
  PY ?= python3
  BIN = $(VENV)/bin
  RM = rm -rf
endif
PIP = $(BIN)/pip
PYTEST = $(BIN)/pytest
VPY = $(BIN)/python

.PHONY: setup test test-fast lint synth bondriver rt rt-vectors rt-test libiio-fetch clean

setup:
	$(PY) -m venv $(VENV)
	$(VPY) -m pip install -U pip
	$(PIP) install -e ".[dev]"

test-fast:
	$(PYTEST) -q -m "not slow"

test:
	$(PYTEST) -q

lint:
	$(BIN)/ruff check ref tools

synth:
	$(VPY) tools/make_synthetic.py --out testdata/synthetic

bondriver:
	powershell -NoProfile -ExecutionPolicy Bypass -File rt/bondriver/build.ps1

# rt/: Windows は build.ps1（vswhere で MSVC を探す）、Linux / macOS は CMake を直接
ifeq ($(OS),Windows_NT)
  RT_TEST = rt/build/rt-x64/Release/rt_test.exe
rt:
	powershell -NoProfile -ExecutionPolicy Bypass -File rt/build.ps1
else
  RT_TEST = rt/build/rt-native/rt_test
rt:
	cmake -S rt -B rt/build/rt-native -DCMAKE_BUILD_TYPE=Release
	cmake --build rt/build/rt-native -j
endif

rt-vectors:
	$(VPY) tools/gen_rt_vectors.py --out testdata/rt_vectors

rt-test:
	$(RT_TEST) testdata/rt_vectors

libiio-fetch:
	powershell -ExecutionPolicy Bypass -File rt/third_party/fetch_libiio.ps1

clean:
	$(RM) .pytest_cache ref/isdbt/__pycache__ ref/isdbt/tables/__pycache__ ref/tests/__pycache__

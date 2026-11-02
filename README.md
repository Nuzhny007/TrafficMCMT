# TrafficMCMT

Master-узел многокамерного трекинга (Multi-Camera Multi-Target, MCMT) для дорожного
трафика. Реализует **late fusion**: принимает треклеты от Edge-узлов
([Multitarget-tracker](https://github.com/Nuzhny007/Multitarget-tracker)), выполняет
межкамерный Re-identification, пространственно-временную фильтрацию и глобальную
ассоциацию через min-cost flow, выдавая стабильные глобальные ID.

## Документация

| Документ | Содержание |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | топология, потоки данных, протокол, алгоритмы, ADR |
| [`docs/roadmap.md`](docs/roadmap.md) | этапы, DoD, тесты, оценки, реестр рисков |

Оба документа — **живые спецификации**: обновляются вместе с кодом.

## Статус

| Этап | Статус |
|---|---|
| 1. Инфраструктура (CMake, конфиг, логи, CLI, тесты) | 🟡 в работе (CI) |
| 2. Протокол и файловый I/O | ✅ готов |
| 3. FAISS и ReID | ⬜ |
| 4. st-ReID | ⬜ |
| 5. muSSP и глобальная ассоциация | ⬜ |
| 6. ZeroMQ транспорт | ⬜ |
| 7. Синхронизация времени | ⬜ |
| 8. BEV | ⬜ |

## Требования

- C++20, CMake ≥ 3.21
- OpenCV 4.12 (только для Edge-моста и BEV; Master собирается CPU-only)
- Protobuf 3.21+ с CMake-таргетами `protobuf::libprotobuf` и `protobuf::protoc`
- [Multitarget-tracker](https://github.com/Nuzhny007/Multitarget-tracker) установленный
  (`MTTracking_DIR` → `<install>/lib/MTTracking/cmake`), собранный **тем же компилятором**,
  что и TrafficMCMT, против той же версии OpenCV, с
  `USE_OCV_KCF=OFF`, `USE_OCV_UKF=OFF`, `USE_OCV_EMBEDDINGS=OFF`
- spdlog, yaml-cpp, GoogleTest (находятся через `find_package`, иначе — `FetchContent`)

Локальная сборка MTT (путь по умолчанию для `CMakeUserPresets.json`):

```powershell
git clone https://github.com/Nuzhny007/Multitarget-tracker.git C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker
cmake -S C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker `
      -B C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker\build `
      -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_INSTALL_PREFIX=C:/work/home/mtracker/TrafficMCMT/Multitarget-tracker/install `
      -DOpenCV_DIR=C:/work/tools/vcpkg/installed/x64-windows/share/opencv4 `
      -DCMAKE_PREFIX_PATH=C:/work/tools/vcpkg/installed/x64-windows `
      -DBUILD_EXAMPLES=OFF -DUSE_OCV_KCF=OFF -DUSE_OCV_UKF=OFF -DUSE_OCV_EMBEDDINGS=OFF
cmake --build C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker\build --config Release --parallel 4
cmake --install C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker\build --config Release
```

## Сборка (Windows / MSVC)

```powershell
cmake --preset local-release
cmake --build --preset local-release
ctest --preset local-release
```

`CMakeUserPresets.json` содержит локальные пути к зависимостям этой машины и не
коммитится. На другой машине задайте пути вручную:

```powershell
cmake -S . -B build/release -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_PREFIX_PATH="<spdlog>;<yaml-cpp>;<gtest>;<opencv>" `
  -DMTTracking_DIR="<MTT install>/lib/MTTracking/cmake"
cmake --build build/release --config Release
ctest --test-dir build/release -C Release --output-on-failure
```

## Сборка (Linux)

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

## Запуск

```bash
trafficmcmt_master --config configs/master.yaml
trafficmcmt_master --config configs/master.yaml --dump-config
trafficmcmt_master --version
trafficmcmt_master --help
```

Экспорт треклетов из видео (Этап 2):

```bash
trafficmcmt_mtt_export --video data/atrium.avi --out data/tracklets.jsonl --speed 0
trafficmcmt_mtt_export --video data/atrium.avi --out data/tracklets.bin --quantize --speed 0
trafficmcmt_tracklet_inspect data/tracklets.jsonl
trafficmcmt_tracklet_inspect data/tracklets.bin --json --limit 5
```

## Структура

```
include/trafficmcmt/core/   доменные типы, конфиг, Result, логирование
include/trafficmcmt/io/     codec, file source/sinks, normalizer, store
include/trafficmcmt/edge/   ReID-экстракторы и EdgeTrackExporter
include/trafficmcmt/mtt/    адаптер Multitarget-tracker → Observation
proto/                      protobuf-схемы v1 (tracklet, calibration)
src/                        реализации core/io/edge/mtt
apps/master_node/           точка входа Master
apps/mtt_export/            видео → треклеты
apps/tracklet_inspect/      инспектор треклетов
tests/unit/                 unit-тесты (GTest)
configs/                    примеры конфигурации
cmake/                      зависимости, warnings, runtime-DLL, package config
docs/                       архитектура и roadmap
```

## Лицензия

См. [LICENSE](LICENSE).

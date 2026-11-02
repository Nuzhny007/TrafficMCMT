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
| 1. Инфраструктура (CMake, конфиг, логи, CLI, тесты) | 🟡 в работе |
| 2. Протокол и файловый I/O | ⬜ |
| 3. FAISS и ReID | ⬜ |
| 4. st-ReID | ⬜ |
| 5. muSSP и глобальная ассоциация | ⬜ |
| 6. ZeroMQ транспорт | ⬜ |
| 7. Синхронизация времени | ⬜ |
| 8. BEV | ⬜ |

## Требования

- C++20, CMake ≥ 3.20
- OpenCV (только для Edge-моста и BEV; Master собирается CPU-only)
- [Multitarget-tracker](https://github.com/Nuzhny007/Multitarget-tracker) установленный
  (`MTTracking_DIR` → `<install>/lib/MTTracking/cmake`)
- spdlog, yaml-cpp, GoogleTest (находятся через `find_package`, иначе — `FetchContent`)

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

## Структура

```
include/trafficmcmt/core/   доменные типы, конфиг, Result, логирование
src/core/                   реализация core
apps/master_node/           точка входа Master
tests/unit/                 unit-тесты (GTest)
configs/                    примеры конфигурации
cmake/                      зависимости, warnings, package config
docs/                       архитектура и roadmap
```

## Лицензия

См. [LICENSE](LICENSE).

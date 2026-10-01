# LivePlay+ — benchmark de importación JSON

Medición local del 1 de octubre de 2026, Release Windows, MSVC 19.51 / SDK 10.0.26100. Phase 1.6 conserva la interfaz `TrackRepository` y no añade SQLite ni dependencias. Los datos Phase 1.5 se conservan en [LIBRARY_BENCHMARK_RESULTS.json](LIBRARY_BENCHMARK_RESULTS.json); la corrida nueva se conserva en [LIBRARY_BENCHMARK_PHASE16_RESULTS.json](LIBRARY_BENCHMARK_PHASE16_RESULTS.json).

## Método

`server/tests/library_benchmark.cpp` genera WAVs únicos de un segundo, PCM estéreo 48 kHz / 16 bits, 192.044 bytes por archivo. Ejecuta 100, 500, 1.000 y 5.000 imports con `ImportService` y `JsonTrackRepository` reales en catálogos vacíos separados. La preparación del fixture no entra en los tiempos. Cada run mide hashing SHA-256 durante copia, metadata, copia, persistencia, catálogo final, pico del journal y pico de memoria de proceso. Para 5.000, los submits se dividen en lotes de 1.000 por el límite de API.

Phase 1.6 escribe cada mutación durable en el journal, coalesce snapshots y hace una compaction explícita final. `persistence_ms` = persistencia de catálogo + append/fsync de journal + compaction. El total incluye submit, colas, serialización, publicación, polling y residual; no es una medición de CPU. Se ejecutó sin E2E ni build concurrente. No se vació la caché del sistema y no mide HTTP upload, canciones comprimidas, antivirus, discos distintos ni percentiles.

## Resultados Phase 1.5 frente a Phase 1.6

Tiempos en segundos; delta total positivo indica mejora. `—` no se midió en Phase 1.5.

| Tracks | Persistencia 1.5 | Persistencia 1.6 | Total 1.5 | Total 1.6 | Delta total | Catálogo 1.6 | Journal pico | Pico memoria |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 0,927 | 0,557 | 1,298 | 1,586 | -22,2 % | 63.645 B | 56.172 B | 9,04 MB |
| 500 | 8,345 | 2,442 | 11,314 | 8,212 | +27,4 % | 318.845 B | 282.172 B | 13,39 MB |
| 1.000 | 26,887 | 5,213 | 35,603 | 22,393 | +37,1 % | 637.846 B | 564.673 B | 18,74 MB |
| 5.000 | — | 23,259 | — | 93,501 | — | 3.197.846 B | 568.000 B | 54,53 MB |

Desglose Phase 1.6 (segundos):

| Tracks | SHA-256 | Metadata | Copia | Catálogo | Journal | Compaction | Persistencia | Total | Fallos |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 0,212 | 0,086 | 0,177 | 0,458 | 0,072 | 0,027 | 0,557 | 1,586 | 0 |
| 500 | 1,078 | 0,279 | 0,667 | 2,054 | 0,352 | 0,036 | 2,442 | 8,212 | 0 |
| 1.000 | 2,136 | 0,645 | 1,461 | 3,970 | 1,182 | 0,062 | 5,213 | 22,393 | 0 |
| 5.000 | 10,161 | 2,589 | 5,899 | 17,746 | 4,653 | 0,861 | 23,259 | 93,501 | 0 |

El journal reduce la persistencia de 1.000 pistas en 80,6 % y permite que su coste deje de dominar el total como en Phase 1.5. El coste fijo de apertura, fsync y publicación vuelve el lote de 100 un 22,2 % más lento; es el intercambio explícito por durabilidad ante crash. El pico del journal de 5.000 se mantiene cerca de 568 KB porque cada lote se compacta; catálogo y memoria crecen aproximadamente linealmente. En 5.000, la persistencia aún representa 24,9 % del total; metadata/copia y el residual de la cola son ahora los mayores candidatos de estudio.

No existe evidencia técnica para migrar a SQLite en esta fase. Mantener JSON+journal es razonable hasta que se necesiten índices/consultas que no cubra `TrackRepository`, catálogos mucho mayores o medidas repetidas muestren un incumplimiento de objetivo. Si se evalúa SQLite, comparar el mismo corpus, cold/warm cache, carga concurrente, recuperación de corte y empaquetado Windows antes de migrar.

## Ejecución

```powershell
Set-Location server/build
./liveplay-library-benchmark.exe ../../docs/LIBRARY_BENCHMARK_PHASE16_RESULTS.json
```

El benchmark crea y elimina sólo un directorio único `plus-benchmark-*` bajo el cwd. La prueba de continuidad simultánea está separada en [AUDIO_ISOLATION_TESTS](AUDIO_ISOLATION_TESTS.md).

# Отчёт анализа трейса Perfetto: `trace_2026-11-09_03-33-35.pftrace`
- **Дата анализа:** 2026-11-09 03:35:08
- **Размер трейса:** 26.84 МБ
- **Всего запросов:** 41

## Оглавление
1. [01_Jank_Summary](#query-1)
2. [02_Top_Waits](#query-2)
3. [03_GPU_Fences](#query-3)
4. [04_Thread_CPU](#query-4)
5. [Топ потоков по времени:](#query-5)
6. [Топ функций по perf-сэмплам (общий):](#query-6)
7. [Кто чаще всего вытесняет (preempted)](#query-7)
8. [«Кто сжирал процессор во время этих зависаний»](#query-8)
9. [«Список самых долгих кадров»](#query-9)
10. [Что делает Главный поток (Main Thread](#query-10)
11. [Блокировки (Lock Contention)](#query-11)
12. [Binder-транзакции (Межпроцессное взаимодействие)](#query-12)
13. [Сборка мусора (Garbage Collection)](#query-13)
14. [Частоты процессора ПО КАЖДОМУ ЯДРУ ОТДЕЛЬНО (видно троттлинг Little / Big / Prime](#query-14)
15. [Состояния ожидания (Uninterruptible Sleep / D-State)](#query-15)
16. [На каких ядрах (CPU 0..7) реально крутятся потоки игры (проверка пиннинга)](#query-16)
17. [Причины блокировок потоков игры (Uninterruptible Sleep & Mutex contention)](#query-17)
18. [SELECT t.name, s.cpu, ROUND(SUM(s.dur)/1e6,1) AS ms FROM sched_slice s JOIN thread t USING(utid) WHERE t.name IN ('RenderGuest','RageStream','GtaRecorder') OR t.name LIKE 'XMA%' GROUP BY t.name, s.cpu ORDER BY t.name, ms DESC;](#query-18)
19. [Топ потоков по CPU за весь трейс](#query-19)
20. [Длинные Running-слайсы (>15 ms) — микрофризы](#query-20)
21. [Frame timeline (если есть)](#query-21)
22. [Потоки Audio / Rage / Recorder](#query-22)
23. [Lock-query (уже с utid):](#query-23)
24. [00. Sanity Check (stats & errors)](#query-24)
25. [00. Package profileable check](#query-25)
26. [00. Ftrace setup errors](#query-26)
27. [01. Ритм кадров (FPS и задержки) по маркерам Rec: FinishPresent](#query-27)
28. [02. Маркеры профилирования игры: Rec: / RW: / RG: (Backpressure)](#query-28)
29. [03. Фактические частоты CPU по кластерам (Little / Big / Prime)](#query-29)
30. [04. Троттлинг: падения потолка частоты CPU (power/cpu_frequency_limits)](#query-30)
31. [05. Причина падения частоты: температура и cdev за 1 сек до троттлинга](#query-31)
32. [06. Размещение потоков игры по ядрам и миграции между кластерами](#query-32)
33. [07. Время в очереди на CPU (Runnable / Preempted) по игровым потокам](#query-33)
34. [08. Кто будит RenderGuest / GtaRecorder / GtaRenderWork (waker поток)](#query-34)
35. [09. Распределение снов (S-state) и подозрение на пересып (TimerSlack)](#query-35)
36. [10. Idle-состояния процессора (активность против глубокого сна)](#query-36)
37. [11. Частота GPU (Adreno devfreq / kgsl)](#query-37)
38. [12. Потребление батареи (ток разряда/заряда)](#query-38)
39. [13. Горячие функции GtaRecorder (Callstack Sampling через linux.perf)](#query-39)
40. [14. Сэмплы функций во время тяжелых лагов (интервалы FinishPresent > 33.4 мс)](#query-40)
41. [15. Логи игры, OPLUS и Thermal на едином таймлайне (android.log)](#query-41)

---

<a id='query-1'></a>
### 01. 01_Jank_Summary
<details><summary>Показать SQL</summary>

```sql
SELECT name AS jank_type, COUNT(*) AS events, ROUND(AVG(dur) / 1e6, 2) AS avg_ms, ROUND(MAX(dur) / 1e6, 2) AS max_ms, ROUND(SUM(dur) / 1e6, 2) AS total_ms FROM slice WHERE name LIKE '%Deadline Missed%' OR name LIKE '%Jank%' GROUP BY name ORDER BY total_ms DESC;
```
</details>

*Время выполнения:* **0.06 сек** | *Строк:* **0**

*0 строк возвращено*


---

<a id='query-2'></a>
### 02. 02_Top_Waits
<details><summary>Показать SQL</summary>

```sql
SELECT name, COUNT(*) AS slice_count, ROUND(SUM(dur) / 1e6, 2) AS total_ms, ROUND(AVG(dur) / 1e6, 2) AS avg_ms FROM slice WHERE name LIKE 'waiting for %' OR name = 'waitForever' GROUP BY name ORDER BY total_ms DESC LIMIT 30;
```
</details>

*Время выполнения:* **0.02 сек** | *Строк:* **30**

| name | slice_count | total_ms | avg_ms |
| --- | --- | --- | --- |
| waitForever | 1117 | 25541.47 | 22.87 |
| waiting for presentFence 708 | 123 | 3999.23 | 32.51 |
| waiting for presentFence 706 | 124 | 3990.95 | 32.19 |
| waiting for presentFence 707 | 124 | 3964.56 | 31.97 |
| waiting for GPU completion 66 | 1 | 39.64 | 39.64 |
| waiting for GPU completion 157 | 1 | 39.25 | 39.25 |
| waiting for GPU completion 87 | 1 | 39.06 | 39.06 |
| waiting for GPU completion 94 | 1 | 38.95 | 38.95 |
| waiting for GPU completion 75 | 1 | 38.88 | 38.88 |
| waiting for GPU completion 156 | 1 | 38.88 | 38.88 |
| waiting for GPU completion 192 | 1 | 38.85 | 38.85 |
| waiting for GPU completion 161 | 1 | 38.83 | 38.83 |
| waiting for GPU completion 22 | 1 | 38.77 | 38.77 |
| waiting for GPU completion 187 | 1 | 38.73 | 38.73 |
| waiting for GPU completion 222 | 1 | 38.69 | 38.69 |
| waiting for GPU completion 59 | 1 | 38.68 | 38.68 |
| waiting for GPU completion 345 | 1 | 38.61 | 38.61 |
| waiting for GPU completion 233 | 1 | 38.59 | 38.59 |
| waiting for GPU completion 310 | 1 | 38.57 | 38.57 |
| waiting for GPU completion 333 | 1 | 38.55 | 38.55 |
| waiting for GPU completion 162 | 1 | 38.54 | 38.54 |
| waiting for GPU completion 184 | 1 | 38.52 | 38.52 |
| waiting for GPU completion 60 | 1 | 38.46 | 38.46 |
| waiting for GPU completion 58 | 1 | 38.46 | 38.46 |
| waiting for GPU completion 317 | 1 | 38.46 | 38.46 |
| waiting for GPU completion 280 | 1 | 38.42 | 38.42 |
| waiting for GPU completion 234 | 1 | 38.41 | 38.41 |
| waiting for GPU completion 131 | 1 | 38.38 | 38.38 |
| waiting for GPU completion 95 | 1 | 38.35 | 38.35 |
| waiting for GPU completion 239 | 1 | 38.33 | 38.33 |

---

<a id='query-3'></a>
### 03. 03_GPU_Fences
<details><summary>Показать SQL</summary>

```sql
SELECT ct.name AS counter_name, ROUND(MIN(c.value), 2) AS min_value, ROUND(MAX(c.value), 2) AS max_value, COUNT(c.id) AS samples FROM counter c JOIN counter_track ct ON c.track_id = ct.id WHERE ct.name LIKE '%presentFence%' OR ct.name LIKE '%GPU completion%' OR ct.name LIKE '%RE completion%' GROUP BY ct.name;
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **2**

| counter_name | min_value | max_value | samples |
| --- | --- | --- | --- |
| GPU completion | 0.0 | 1.0 | 745 |
| presentFence | 0.0 | 1.0 | 744 |

---

<a id='query-4'></a>
### 04. 04_Thread_CPU
<details><summary>Показать SQL</summary>

```sql
SELECT t.name AS thread_name, ts.state, ROUND(SUM(ts.dur) / 1e6, 2) AS total_time_ms, COUNT(*) AS state_changes FROM thread_state ts JOIN thread t ON ts.utid = t.utid WHERE ts.state = 'Running' OR ts.state = 'R' GROUP BY t.name, ts.state ORDER BY total_time_ms DESC LIMIT 30;
```
</details>

*Время выполнения:* **0.99 сек** | *Строк:* **30**

| thread_name | state | total_time_ms | state_changes |
| --- | --- | --- | --- |
| RenderGuest | Running | 19033.3 | 1021 |
| RageStream | Running | 10328.52 | 12184 |
| AudioGuest | Running | 7233.28 | 2976 |
| Main XThread (F | Running | 6531.67 | 2675 |
| RageJobWork | R | 4179.84 | 46471 |
| traced_perf | Running | 3321.32 | 3474 |
| RageJobWork | Running | 3214.55 | 49305 |
| stack-unwinding | Running | 2426.67 | 1958 |
| Rage_556188 | R | 2264.66 | 17439 |
| SDLThread | Running | 2088.61 | 20534 |
| SDLThread | R | 1958.42 | 19304 |
| RagePhysics | Running | 1915.62 | 6182 |
| traced_probes | Running | 1869.74 | 507 |
| writer | Running | 1813.48 | 14451 |
| RagePhysics | R | 1774.49 | 5763 |
| SDLActivity | R | 1646.82 | 21364 |
| surfaceflinger | Running | 1446.81 | 2522 |
| FastMixer | Running | 1400.3 | 9643 |
| Rage_299490 | Running | 1304.15 | 2436 |
| SDLActivity | Running | 1294.98 | 21615 |
| GtaCmdRetirer | Running | 1151.05 | 1136 |
| Rage_556188 | Running | 987.42 | 17608 |
| RageStream | R | 977.58 | 12175 |
| bluetooth@1.0-s | Running | 941.29 | 3908 |
| kworker/u24:5 | Running | 790.47 | 4264 |
| HwBinder:565_4 | Running | 759.76 | 2296 |
| kworker/u24:3 | Running | 665.33 | 3797 |
| Audio Worker (F | R | 658.06 | 15633 |
| writer | R | 653.97 | 14401 |
| Rage_3A05E8 | Running | 533.9 | 1780 |

---

<a id='query-5'></a>
### 05. Топ потоков по времени:
<details><summary>Показать SQL</summary>

```sql
SELECT 
  t.name AS thread_name,
  ROUND(SUM(s.dur)/1e6, 1) AS total_ms
FROM sched_slice s
JOIN thread t USING(utid)
WHERE t.name IS NOT NULL
GROUP BY t.name
ORDER BY total_ms DESC
LIMIT 15;
```
</details>

*Время выполнения:* **0.97 сек** | *Строк:* **15**

| thread_name | total_ms |
| --- | --- |
| swapper | 74032.6 |
| RenderGuest | 19033.3 |
| RageStream | 10328.5 |
| AudioGuest | 7233.3 |
| Main XThread (F | 6531.7 |
| traced_perf | 3321.3 |
| RageJobWork | 3214.5 |
| stack-unwinding | 2426.7 |
| SDLThread | 2088.6 |
| RagePhysics | 1915.6 |
| traced_probes | 1869.7 |
| writer | 1813.5 |
| surfaceflinger | 1446.8 |
| FastMixer | 1400.3 |
| Rage_299490 | 1304.2 |

---

<a id='query-6'></a>
### 06. Топ функций по perf-сэмплам (общий):
<details><summary>Показать SQL</summary>

```sql
SELECT
  spf.name AS frame,
  COUNT(*) AS samples
FROM perf_sample ps
JOIN stack_profile_callsite spc ON ps.callsite_id = spc.id
JOIN stack_profile_frame spf ON spc.frame_id = spf.id
GROUP BY spf.name
ORDER BY samples DESC
LIMIT 25;
```
</details>

*Время выполнения:* **0.03 сек** | *Строк:* **25**

| frame | samples |
| --- | --- |
| syscall | 660 |
|  | 465 |
| __imp__sub_829321A0 | 396 |
| sub_829134C8 | 391 |
| sub_82914758 | 228 |
| _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem19CaptureTitleCommandEjPKvmPKNS2_13CaptureReplayE | 221 |
| __aarch64_cas2_acq | 220 |
| _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem21RecordNativeFrameTaskEP17VkCommandBuffer_TRKjS6_RKP9VkImage_TRKP13VkImageView_TjRKNSt6__ndk110shared_ptrIKNS2_21NativeTextureResourceEEERKNSG_IKNS1_19EnvironmentalDataV2EEERKbRKfRbSV_SS_S6_SS_b.resume | 209 |
| _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem13NativeCommand15RetireResourcesEv | 205 |
| sub_82199BC8 | 200 |
| _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem21CaptureBufferResourceEj | 198 |
| __memcpy_aarch64_simd | 170 |
| __imp__sub_82919810 | 170 |
| _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem24PrepareFrameTexturesTaskEP17VkCommandBuffer_Tbjbb.resume | 168 |
| sub_829055A8 | 164 |
| nanosleep | 156 |
| _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem16SnapshotPipelineERKNS2_13NativeCommandEb | 146 |
| sub_82935BD0 | 133 |
| sub_821997F8 | 126 |
| _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem16RenderWorkerLoopENS2_14WorkerLoopModeE | 123 |
| sub_82936CE0 | 120 |
| sub_82912FC0 | 119 |
| _ZNK3rex6thread18PosixConditionBase19SpinUntilEpochMovesEjjPKNSt6__ndk16atomicIbEEb | 118 |
| sub_82908D98 | 107 |
| sub_821D5F70 | 107 |

---

<a id='query-7'></a>
### 07. Кто чаще всего вытесняет (preempted)
<details><summary>Показать SQL</summary>

```sql
SELECT
  t.name AS thread_name,
  COUNT(*) AS preempt_count,
  ROUND(SUM(ts.dur)/1e6, 1) AS preempted_ms
FROM thread_state ts
JOIN thread t USING(utid)
WHERE state = 'R+' OR state = 'Runnable (Preempted)'
GROUP BY t.name
ORDER BY preempted_ms DESC
LIMIT 15;
```
</details>

*Время выполнения:* **0.02 сек** | *Строк:* **15**

| thread_name | preempt_count | preempted_ms |
| --- | --- | --- |
| SDLThread | 1225 | 177.5 |
| traced_perf | 536 | 125.1 |
| kworker/u24:3 | 463 | 106.7 |
| kworker/u24:5 | 535 | 101.4 |
| kworker/u24:0 | 315 | 69.5 |
| RagePhysics | 141 | 58.0 |
| HwBinder:565_4 | 336 | 51.9 |
| kworker/u24:1 | 230 | 48.5 |
| RageJobWork | 323 | 45.9 |
| traced_probes | 195 | 31.8 |
| SDLActivity | 254 | 31.7 |
| stack-unwinding | 167 | 28.9 |
| SensorService | 99 | 23.1 |
| Rage_556188 | 165 | 22.7 |
| 11_see | 116 | 22.6 |

---

<a id='query-8'></a>
### 08. «Кто сжирал процессор во время этих зависаний»
<details><summary>Показать SQL</summary>

```sql
WITH RECURSIVE
  spike_frames AS (
    SELECT ts, dur
    FROM actual_frame_timeline_slice
    WHERE dur > 30000000
  ),
  spike_samples AS (
    SELECT ps.utid, ps.callsite_id
    FROM perf_sample ps
    JOIN spike_frames f
      ON ps.ts >= f.ts AND ps.ts < (f.ts + f.dur)
  ),
  expanded_stacks AS (
    SELECT utid, callsite_id AS cid
    FROM spike_samples
    UNION ALL
    SELECT es.utid, spc.parent_id
    FROM expanded_stacks es
    JOIN stack_profile_callsite spc ON es.cid = spc.id
    WHERE spc.parent_id IS NOT NULL
  )
SELECT
  thread.name AS thread_name,
  spf.name AS symbol_name,
  COUNT(*) AS hits_in_spikes
FROM expanded_stacks es
JOIN stack_profile_callsite spc ON es.cid = spc.id
JOIN stack_profile_frame spf ON spc.frame_id = spf.id
JOIN thread USING (utid)
WHERE spf.name NOT LIKE '__start_thread%'
  AND spf.name NOT LIKE '__pthread_start%'
GROUP BY thread_name, symbol_name
ORDER BY hits_in_spikes DESC
LIMIT 35;
```
</details>

*Время выполнения:* **1.43 сек** | *Строк:* **35**

| thread_name | symbol_name | hits_in_spikes |
| --- | --- | --- |
| RenderGuest | _ZL15__pthread_startPv | 3188 |
| RenderGuest | _ZN3rex6system7XThread7ExecuteEv | 3187 |
| RenderGuest | _ZN3rex6thread14PosixConditionINS0_6ThreadEE18ThreadStartRoutineEPv | 3187 |
| RenderGuest | _ZNSt6__ndk110__function6__funcIZN3rex6system7XThread6CreateEvE3$_0NS_9allocatorIS5_EEFvvEEclEv | 3187 |
| RenderGuest | __imp__sub_82A21600 | 3187 |
| RenderGuest | sub_821B5B20 | 3187 |
| RenderGuest | sub_821B6F18 | 3187 |
| RenderGuest | sub_82849940 | 3187 |
| RenderGuest | __imp__sub_821BA928 | 3181 |
| RenderGuest | sub_821BB2D0 | 3181 |
| RenderGuest | __imp__sub_821BB2D0 | 3175 |
| RageStream | sub_8290B4F0 | 2963 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem19CaptureTitleCommandEjPKvmPKNS2_13CaptureReplayE | 2434 |
| RenderGuest | __imp__sub_828E7000 | 2210 |
| RenderGuest | __imp__sub_828BFC00 | 2196 |
| RenderGuest | sub_82A3E348 | 2036 |
| RenderGuest | sub_828C42F0 | 1963 |
| RageStream | _ZL15__pthread_startPv | 1800 |
| RageStream | _ZN3rex6system7XThread7ExecuteEv | 1800 |
| RageStream | _ZN3rex6thread14PosixConditionINS0_6ThreadEE18ThreadStartRoutineEPv | 1800 |
| RageStream | _ZNSt6__ndk110__function6__funcIZN3rex6system7XThread6CreateEvE3$_0NS_9allocatorIS5_EEFvvEEclEv | 1800 |
| RageStream | __imp__sub_82A21600 | 1800 |
| RageStream | sub_82849940 | 1800 |
| RageStream | sub_82902420 | 1800 |
| RageStream | sub_82902728 | 1800 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem16PumpDirectRecordEv | 1627 |
| RenderGuest | sub_821BB6D8 | 1626 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem16RenderWorkerLoopENS2_14WorkerLoopModeE | 1618 |
| RenderGuest | sub_828C4338 | 1614 |
| RenderGuest | __imp__sub_828C4338 | 1611 |
| RenderGuest | sub_828C6620 | 1600 |
| RenderGuest | __imp__sub_828C6620 | 1589 |
| RageStream | sub_8291FA80 | 1336 |
| RageStream | __imp__sub_829225E0 | 1294 |
| AudioGuest | _ZL15__pthread_startPv | 1275 |

---

<a id='query-9'></a>
### 09. «Список самых долгих кадров»
<details><summary>Показать SQL</summary>

```sql
SELECT id, ts, ROUND(dur / 1e6, 2) AS frame_ms, jank_tag
FROM actual_frame_timeline_slice
WHERE dur > 30000000
ORDER BY dur DESC
LIMIT 30;
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **30**

| id | ts | frame_ms | jank_tag |
| --- | --- | --- | --- |
| 33896 | 1769219259825316 | 48.89 | Self Jank |
| 47945 | 1769223213696201 | 48.82 | Self Jank |
| 27762 | 1769217548861620 | 48.8 | Self Jank |
| 21826 | 1769215871032089 | 48.78 | Self Jank |
| 40724 | 1769221170392764 | 48.78 | Self Jank |
| 128 | 1769209741430789 | 48.73 | Self Jank |
| 73 | 1769209492218863 | 48.72 | Self Jank |
| 40903 | 1769221220234639 | 48.72 | Self Jank |
| 43964 | 1769222084006670 | 48.69 | Self Jank |
| 16629 | 1769214442293183 | 48.69 | Self Jank |
| 33715 | 1769219209982661 | 48.68 | Self Jank |
| 16809 | 1769214492215371 | 48.66 | Self Jank |
| 51 | 1769209392573236 | 48.66 | Self Jank |
| 38200 | 1769220456092295 | 48.66 | Self Jank |
| 24894 | 1769216751315162 | 48.65 | Self Jank |
| 18 | 1769209243046623 | 48.65 | Self Jank |
| 45409 | 1769222499311825 | 48.64 | Self Jank |
| 16450 | 1769214392496100 | 48.64 | Self Jank |
| 21102 | 1769215671707818 | 48.64 | Self Jank |
| 62 | 1769209442402665 | 48.64 | Self Jank |
| 45597 | 1769222549185628 | 48.63 | Self Jank |
| 41262 | 1769221320054535 | 48.63 | Self Jank |
| 84 | 1769209542160946 | 48.62 | Self Jank |
| 54977 | 1769225207649325 | 48.62 | Self Jank |
| 885 | 1769210090362872 | 48.62 | Self Jank |
| 34612 | 1769219459224795 | 48.61 | Self Jank |
| 7355 | 1769211884296205 | 48.59 | Self Jank |
| 60516 | 1769226935502970 | 48.59 | Self Jank |
| 62541 | 1769227583369741 | 48.59 | Self Jank |
| 65316 | 1769228463897866 | 48.59 | Self Jank |

---

<a id='query-10'></a>
### 10. Что делает Главный поток (Main Thread
<details><summary>Показать SQL</summary>

```sql
SELECT 
    s.name AS slice_name, 
    COUNT(*) AS count, 
    ROUND(SUM(s.dur) / 1e6, 2) AS total_ms, 
    ROUND(AVG(s.dur) / 1e6, 2) AS avg_ms
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t ON tt.utid = t.utid
WHERE t.name LIKE '%Main%' OR t.name LIKE '%UI%'
GROUP BY s.name
ORDER BY total_ms DESC
LIMIT 20;
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **0**

*0 строк возвращено*


---

<a id='query-11'></a>
### 11. Блокировки (Lock Contention)
<details><summary>Показать SQL</summary>

```sql
SELECT
  t.name AS thread_name,
  s.name AS lock_slice,
  COUNT(*) AS count,
  ROUND(SUM(s.dur) / 1e6, 2) AS total_wait_ms
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t ON tt.utid = t.utid
WHERE s.name LIKE '%locked%'
   OR s.name LIKE '%contention%'
   OR s.name LIKE '%waiting for%'
GROUP BY thread_name, lock_slice
ORDER BY total_wait_ms DESC
LIMIT 20;
```
</details>

*Время выполнения:* **0.06 сек** | *Строк:* **20**

| thread_name | lock_slice | count | total_wait_ms |
| --- | --- | --- | --- |
| presentFence | waiting for presentFence 708 | 123 | 3999.23 |
| presentFence | waiting for presentFence 706 | 124 | 3990.95 |
| presentFence | waiting for presentFence 707 | 124 | 3964.56 |
| binder:18983_2 | releaseBufferCallbackLocked | 420 | 58.77 |
| binder:18983_4 | releaseBufferCallbackLocked | 324 | 50.53 |
| GPU completion | waiting for GPU completion 66 | 1 | 39.64 |
| GPU completion | waiting for GPU completion 157 | 1 | 39.25 |
| SDLThread | applyGrallocMetadataLocked | 372 | 39.11 |
| GPU completion | waiting for GPU completion 87 | 1 | 39.06 |
| GPU completion | waiting for GPU completion 94 | 1 | 38.95 |
| GPU completion | waiting for GPU completion 156 | 1 | 38.88 |
| GPU completion | waiting for GPU completion 75 | 1 | 38.88 |
| GPU completion | waiting for GPU completion 192 | 1 | 38.85 |
| GPU completion | waiting for GPU completion 161 | 1 | 38.83 |
| GPU completion | waiting for GPU completion 22 | 1 | 38.77 |
| GPU completion | waiting for GPU completion 187 | 1 | 38.73 |
| GPU completion | waiting for GPU completion 222 | 1 | 38.69 |
| GPU completion | waiting for GPU completion 59 | 1 | 38.68 |
| GPU completion | waiting for GPU completion 345 | 1 | 38.61 |
| GPU completion | waiting for GPU completion 233 | 1 | 38.59 |

---

<a id='query-12'></a>
### 12. Binder-транзакции (Межпроцессное взаимодействие)
<details><summary>Показать SQL</summary>

```sql
SELECT 
    t.name AS thread_name,
    s.name AS slice_name,
    ROUND(SUM(s.dur) / 1e6, 2) AS total_ms,
    COUNT(*) AS tx_count
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t ON tt.utid = t.utid
WHERE s.name LIKE '%binder transaction%'
GROUP BY t.name, s.name
ORDER BY total_ms DESC
LIMIT 15;
```
</details>

*Время выполнения:* **0.01 сек** | *Строк:* **0**

*0 строк возвращено*


---

<a id='query-13'></a>
### 13. Сборка мусора (Garbage Collection)
<details><summary>Показать SQL</summary>

```sql
SELECT 
    t.name AS thread_name,
    COUNT(*) AS gc_count,
    ROUND(SUM(s.dur) / 1e6, 2) AS total_gc_ms,
    ROUND(MAX(s.dur) / 1e6, 2) AS max_gc_ms
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t ON tt.utid = t.utid
WHERE s.name LIKE '%GC%' OR s.name LIKE '%Garbage%' OR s.name LIKE '%HeapTaskDaemon%'
GROUP BY thread_name
ORDER BY total_gc_ms DESC;
```
</details>

*Время выполнения:* **0.05 сек** | *Строк:* **0**

*0 строк возвращено*


---

<a id='query-14'></a>
### 14. Частоты процессора ПО КАЖДОМУ ЯДРУ ОТДЕЛЬНО (видно троттлинг Little / Big / Prime
<details><summary>Показать SQL</summary>

```sql
SELECT 
    cct.cpu AS core_id,
    ROUND(MIN(c.value) / 1000.0, 0) AS min_mhz,
    ROUND(MAX(c.value) / 1000.0, 0) AS max_mhz,
    ROUND(AVG(c.value) / 1000.0, 0) AS avg_mhz
FROM counter c
JOIN cpu_counter_track cct ON c.track_id = cct.id
WHERE cct.name = 'cpufreq'
GROUP BY cct.cpu
ORDER BY cct.cpu;
```
</details>

*Время выполнения:* **0.01 сек** | *Строк:* **8**

| core_id | min_mhz | max_mhz | avg_mhz |
| --- | --- | --- | --- |
| 0 | 499.0 | 1805.0 | 1586.0 |
| 1 | 499.0 | 1805.0 | 1586.0 |
| 2 | 499.0 | 1805.0 | 1586.0 |
| 3 | 499.0 | 1805.0 | 1586.0 |
| 4 | 710.0 | 2419.0 | 1776.0 |
| 5 | 710.0 | 2419.0 | 1776.0 |
| 6 | 710.0 | 2419.0 | 1776.0 |
| 7 | 845.0 | 2842.0 | 2157.0 |

---

<a id='query-15'></a>
### 15. Состояния ожидания (Uninterruptible Sleep / D-State)
<details><summary>Показать SQL</summary>

```sql
SELECT 
    t.name AS thread_name,
    ts.state,
    COUNT(*) AS count,
    ROUND(SUM(ts.dur) / 1e6, 2) AS total_time_ms
FROM thread_state ts
JOIN thread t ON ts.utid = t.utid
WHERE ts.state = 'D' OR ts.state = 'Uninterruptible'
GROUP BY thread_name, ts.state
ORDER BY total_time_ms DESC
LIMIT 15;
```
</details>

*Время выполнения:* **0.02 сек** | *Строк:* **15**

| thread_name | state | count | total_time_ms |
| --- | --- | --- | --- |
| usbtemp_kthread | D | 1028 | 19041.99 |
| bcc_kthread | D | 10 | 18143.01 |
| shortc_thread | D | 4 | 15862.03 |
| crtc_commit:147 | D | 372 | 2884.55 |
| kworker/3:3 | D | 105 | 199.48 |
| kworker/u24:3 | D | 1586 | 87.04 |
| health@2.1-serv | D | 58 | 86.14 |
| kworker/u24:5 | D | 1685 | 76.5 |
| kworker/u24:0 | D | 1028 | 52.57 |
| st-fitting | D | 14 | 31.8 |
| kworker/u24:1 | D | 619 | 30.97 |
| vendor.oplus.ha | D | 45 | 26.14 |
| stack-unwinding | D | 5 | 11.41 |
| kworker/u24:4 | D | 182 | 9.5 |
| kworker/X26:2 | D | 294 | 9.1 |

---

<a id='query-16'></a>
### 16. На каких ядрах (CPU 0..7) реально крутятся потоки игры (проверка пиннинга)
<details><summary>Показать SQL</summary>

```sql
SELECT 
    t.name AS thread_name,
    s.cpu AS cpu_core,
    ROUND(SUM(s.dur) / 1e6, 1) AS total_ms
FROM sched_slice s
JOIN thread t USING(utid)
WHERE t.name IN ('RenderGuest', 'GtaRecorder', 'GtaRenderWork', 'Main XThread (F', 'AudioGuest', 'XMA Decoder (F8', 'RageStream', 'RageJobWork', 'GtaCmdRetirer', 'AudioTrack')
GROUP BY t.name, s.cpu
ORDER BY t.name, total_ms DESC;
```
</details>

*Время выполнения:* **0.08 сек** | *Строк:* **28**

| thread_name | cpu_core | total_ms |
| --- | --- | --- |
| AudioGuest | 5 | 3789.8 |
| AudioGuest | 4 | 3443.5 |
| AudioTrack | 4 | 32.5 |
| AudioTrack | 5 | 31.9 |
| AudioTrack | 6 | 20.1 |
| AudioTrack | 7 | 0.3 |
| GtaCmdRetirer | 3 | 321.3 |
| GtaCmdRetirer | 0 | 319.5 |
| GtaCmdRetirer | 1 | 275.3 |
| GtaCmdRetirer | 2 | 235.0 |
| Main XThread (F | 6 | 4778.4 |
| Main XThread (F | 4 | 927.6 |
| Main XThread (F | 5 | 740.0 |
| Main XThread (F | 7 | 85.6 |
| RageJobWork | 5 | 1299.7 |
| RageJobWork | 4 | 1268.9 |
| RageJobWork | 0 | 177.4 |
| RageJobWork | 2 | 174.1 |
| RageJobWork | 1 | 153.7 |
| RageJobWork | 3 | 140.8 |
| RageStream | 2 | 2747.5 |
| RageStream | 3 | 2678.4 |
| RageStream | 0 | 2637.4 |
| RageStream | 1 | 2265.2 |
| RenderGuest | 7 | 16903.8 |
| RenderGuest | 6 | 2129.5 |
| XMA Decoder (F8 | 5 | 7.2 |
| XMA Decoder (F8 | 4 | 6.0 |

---

<a id='query-17'></a>
### 17. Причины блокировок потоков игры (Uninterruptible Sleep & Mutex contention)
<details><summary>Показать SQL</summary>

```sql
SELECT
    t.name AS thread_name,
    ts.state,
    ts.io_wait,
    COUNT(*) AS count,
    ROUND(SUM(ts.dur) / 1e6, 2) AS total_ms
FROM thread_state ts
JOIN thread t USING(utid)
WHERE t.name IN ('RenderGuest', 'GtaRecorder', 'GtaRenderWork', 'Main XThread (F', 'AudioGuest', 'RageStream', 'RageFileIO')
  AND ts.state IN ('D', 'S', 'R', 'R+')
GROUP BY t.name, ts.state, ts.io_wait
ORDER BY total_ms DESC
LIMIT 25;
```
</details>

*Время выполнения:* **0.89 сек** | *Строк:* **15**

| thread_name | state | io_wait | count | total_ms |
| --- | --- | --- | --- | --- |
| RenderGuest | S |  | 889 | 42664.29 |
| RageFileIO | S |  | 1924 | 38255.18 |
| Main XThread (F | S |  | 2618 | 12661.72 |
| AudioGuest | S |  | 2117 | 11816.88 |
| RageStream | S |  | 9196 | 7972.48 |
| RageStream | R |  | 12175 | 977.58 |
| RenderGuest | R |  | 730 | 299.69 |
| AudioGuest | R |  | 1973 | 192.18 |
| RageFileIO | R |  | 1954 | 154.35 |
| Main XThread (F | R |  | 2306 | 68.16 |
| AudioGuest | R+ |  | 702 | 14.37 |
| RageFileIO | R+ |  | 37 | 6.82 |
| RageStream | R+ |  | 10 | 2.16 |
| Main XThread (F | R+ |  | 5 | 0.52 |
| RenderGuest | R+ |  | 6 | 0.37 |

---

<a id='query-18'></a>
### 18. SELECT t.name, s.cpu, ROUND(SUM(s.dur)/1e6,1) AS ms FROM sched_slice s JOIN thread t USING(utid) WHERE t.name IN ('RenderGuest','RageStream','GtaRecorder') OR t.name LIKE 'XMA%' GROUP BY t.name, s.cpu ORDER BY t.name, ms DESC;
<details><summary>Показать SQL</summary>

```sql
SELECT t.name, s.cpu, ROUND(SUM(s.dur)/1e6,1) AS ms
FROM sched_slice s JOIN thread t USING(utid)
WHERE t.name IN ('RenderGuest','RageStream','GtaRecorder') OR t.name LIKE 'XMA%'
GROUP BY t.name, s.cpu ORDER BY t.name, ms DESC;
```
</details>

*Время выполнения:* **0.29 сек** | *Строк:* **8**

| name | cpu | ms |
| --- | --- | --- |
| RageStream | 2 | 2747.5 |
| RageStream | 3 | 2678.4 |
| RageStream | 0 | 2637.4 |
| RageStream | 1 | 2265.2 |
| RenderGuest | 7 | 16903.8 |
| RenderGuest | 6 | 2129.5 |
| XMA Decoder (F8 | 5 | 7.2 |
| XMA Decoder (F8 | 4 | 6.0 |

---

<a id='query-19'></a>
### 19. Топ потоков по CPU за весь трейс
<details><summary>Показать SQL</summary>

```sql
SELECT
  process.name AS process,
  thread.name AS thread,
  SUM(dur) / 1e6 AS cpu_ms
FROM sched
JOIN thread USING (utid)
JOIN process USING (upid)
WHERE process.name LIKE '%liberty%' OR process.name LIKE '%com.liberty%'
GROUP BY 1, 2
ORDER BY cpu_ms DESC
LIMIT 30;
```
</details>

*Время выполнения:* **0.60 сек** | *Строк:* **30**

| process | thread | cpu_ms |
| --- | --- | --- |
| com.libertyrecomp:game | RenderGuest | 19033.295819 |
| com.libertyrecomp:game | RageStream | 10328.521209 |
| com.libertyrecomp:game | AudioGuest | 7233.279187 |
| com.libertyrecomp:game | Main XThread (F | 6531.674654 |
| com.libertyrecomp:game | RageJobWork | 3214.545287 |
| com.libertyrecomp:game | SDLThread | 2088.607735 |
| com.libertyrecomp:game | RagePhysics | 1915.62086 |
| com.libertyrecomp:game | Rage_299490 | 1304.152396 |
| com.libertyrecomp:game | SDLActivity | 1294.982285 |
| com.libertyrecomp:game | GtaCmdRetirer | 1151.053546 |
| com.libertyrecomp:game | Rage_556188 | 987.416646 |
| com.libertyrecomp:game | Rage_3A05E8 | 533.903514 |
| com.libertyrecomp:game | Audio Worker (F | 267.145377 |
| com.libertyrecomp:game | Rage_1BA088 | 177.682698 |
| com.libertyrecomp:game | binder:18983_2 | 138.103657 |
| com.libertyrecomp:game | binder:18983_4 | 111.689124 |
| com.libertyrecomp:game | RageFileIO | 106.757585 |
| com.libertyrecomp:game | Rage_9FF398 | 87.470383 |
| com.libertyrecomp:game | Rage_5FBB68 | 86.72562 |
| com.libertyrecomp:game | AudioTrack | 84.88425 |
| com.libertyrecomp:game | GPU completion | 71.205258 |
| com.libertyrecomp:game | Rage_212F38 | 44.247908 |
| com.libertyrecomp:game | SDLAudioP15 | 42.023393 |
| com.libertyrecomp:game | binder:18983_3 | 21.104947 |
| com.libertyrecomp:game | XMA Decoder (F8 | 13.230483 |
| com.libertyrecomp | m.libertyrecomp | 1.707759 |
| com.libertyrecomp:game | UIMonitorThread | 1.606563 |
| com.libertyrecomp | Jit thread pool | 1.496719 |
| com.libertyrecomp | binder:18919_1 | 1.218438 |
| com.libertyrecomp:game | Kernel Host Tas | 0.113698 |

---

<a id='query-20'></a>
### 20. Длинные Running-слайсы (>15 ms) — микрофризы
<details><summary>Показать SQL</summary>

```sql
SELECT
  thread.name AS thread,
  dur / 1e6 AS dur_ms,
  ts
FROM sched
JOIN thread USING (utid)
WHERE dur > 15e6
  AND thread.name IS NOT NULL
ORDER BY dur DESC
LIMIT 40;
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **40**

| thread | dur_ms | ts |
| --- | --- | --- |
| swapper | 150.040052 | 1769228456893490 |
| swapper | 97.465573 | 1769228035783438 |
| swapper | 90.904323 | 1769225811284793 |
| swapper | 86.792812 | 1769227038254741 |
| swapper | 79.273229 | 1769225198666356 |
| lowpool[4] | 77.723177 | 1769228356723022 |
| swapper | 73.258177 | 1769226813540366 |
| RenderGuest | 66.738646 | 1769228061255626 |
| RenderGuest | 66.265105 | 1769228457020365 |
| RenderGuest | 65.310886 | 1769227584128230 |
| swapper | 65.20125 | 1769225591043700 |
| RenderGuest | 65.046979 | 1769227047108647 |
| RenderGuest | 64.209844 | 1769227116509845 |
| RenderGuest | 63.915937 | 1769225461487554 |
| RenderGuest | 63.692656 | 1769227995770105 |
| RenderGuest | 63.588229 | 1769225766552762 |
| swapper | 62.577447 | 1769225278434117 |
| RenderGuest | 62.487135 | 1769225267830523 |
| RenderGuest | 62.456771 | 1769225831942866 |
| RenderGuest | 60.706979 | 1769226816644064 |
| RenderGuest | 59.960104 | 1769226244667293 |
| swapper | 59.101823 | 1769227825658907 |
| RenderGuest | 57.570521 | 1769228533228386 |
| BG Thread #3 | 55.49875 | 1769216788926464 |
| swapper | 54.717396 | 1769227980949845 |
| traced_probes | 54.714844 | 1769218693811046 |
| swapper | 54.235781 | 1769225756933127 |
| swapper | 53.72823 | 1769224866752033 |
| RenderGuest | 52.834219 | 1769222646552711 |
| RenderGuest | 52.726458 | 1769228340658230 |
| RenderGuest | 52.275885 | 1769222750320055 |
| traced_probes | 52.222656 | 1769228695438751 |
| RenderGuest | 52.210365 | 1769222802703805 |
| swapper | 52.206198 | 1769228930699896 |
| RenderGuest | 52.187708 | 1769224868446721 |
| RenderGuest | 52.156979 | 1769222233379482 |
| RenderGuest | 52.055156 | 1769222025815941 |
| RenderGuest | 51.842396 | 1769215918881516 |
| RenderGuest | 51.630573 | 1769216488314068 |
| RenderGuest | 51.556459 | 1769222285643180 |

---

<a id='query-21'></a>
### 21. Frame timeline (если есть)
<details><summary>Показать SQL</summary>

```sql
SELECT name, dur / 1e6 AS dur_ms
FROM actual_frame_timeline_slice
ORDER BY dur DESC
LIMIT 30;
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **30**

| name | dur_ms |
| --- | --- |
| 25228220 | 48.885417 |
| 25228298 | 48.820052 |
| 25228186 | 48.797291 |
| 25228153 | 48.783333 |
| 25228258 | 48.779688 |
| 25228023 | 48.725105 |
| 25228018 | 48.723177 |
| 25228259 | 48.716146 |
| 25228276 | 48.6875 |
| 25228124 | 48.686198 |
| 25228219 | 48.679948 |
| 25228125 | 48.664583 |
| 25228016 | 48.664271 |
| 25228244 | 48.661771 |
| 25228170 | 48.654843 |
| 25228013 | 48.64625 |
| 25228284 | 48.643854 |
| 25228123 | 48.643281 |
| 25228149 | 48.642292 |
| 25228017 | 48.638802 |
| 25228285 | 48.625886 |
| 25228261 | 48.625625 |
| 25228019 | 48.624479 |
| 25228337 | 48.620833 |
| 25228030 | 48.616459 |
| 25228224 | 48.605625 |
| 25228066 | 48.593854 |
| 25228387 | 48.593021 |
| 25228398 | 48.592656 |
| 25228413 | 48.590052 |

---

<a id='query-22'></a>
### 22. Потоки Audio / Rage / Recorder
<details><summary>Показать SQL</summary>

```sql
SELECT
  thread.name,
  SUM(CASE WHEN state = 'Running' THEN dur ELSE 0 END) / 1e6 AS running_ms,
  SUM(CASE WHEN state = 'R' OR state LIKE 'R%' THEN dur ELSE 0 END) / 1e6 AS runnable_ms,
  SUM(dur) / 1e6 AS total_ms
FROM thread_state
JOIN thread USING (utid)
WHERE thread.name LIKE '%Audio%'
   OR thread.name LIKE '%Rage%'
   OR thread.name LIKE '%Record%'
   OR thread.name LIKE '%Render%'
   OR thread.name LIKE '%Gta%'
   OR thread.name LIKE '%XThread%'
GROUP BY 1
ORDER BY running_ms DESC
LIMIT 40;
```
</details>

*Время выполнения:* **2.07 сек** | *Строк:* **22**

| name | running_ms | runnable_ms | total_ms |
| --- | --- | --- | --- |
| RenderGuest | 19033.295819 | 19333.360108 | 61997.651847 |
| RageStream | 10328.521209 | 11308.26034 | 19280.73869 |
| AudioGuest | 7233.279187 | 7439.831874 | 19256.715044 |
| Main XThread (F | 6531.674654 | 6600.354459 | 19262.078898 |
| RageJobWork | 3214.545287 | 7440.306828 | 96378.015532 |
| RagePhysics | 1915.62086 | 3748.062031 | 57836.435339 |
| Rage_299490 | 1304.152396 | 1624.428841 | 19290.334784 |
| GtaCmdRetirer | 1151.053546 | 1362.509588 | 19249.929575 |
| Rage_556188 | 987.416646 | 3274.742611 | 19290.983013 |
| Rage_3A05E8 | 533.903514 | 783.642027 | 19286.837492 |
| AudioOut_D | 468.731769 | 521.315615 | 19260.532909 |
| Audio Worker (F | 267.145377 | 930.233392 | 19292.01869 |
| Rage_1BA088 | 177.682698 | 299.540692 | 19287.09171 |
| RageFileIO | 106.757585 | 267.926934 | 38523.111702 |
| Rage_9FF398 | 87.470383 | 253.987107 | 38550.844098 |
| Rage_5FBB68 | 86.72562 | 246.965512 | 19278.583221 |
| AudioTrack | 84.88425 | 120.741698 | 19250.951711 |
| Rage_212F38 | 44.247908 | 122.994897 | 19268.074575 |
| SDLAudioP15 | 42.023393 | 92.838872 | 19249.832544 |
| RenderThread | 5.22021 | 9.166458 | 30.136873 |
| khungtaskd | 5.049166 | 5.534323 | 5.534322 |
| DeviceStorageMo | 0.656562 | 0.695781 | 0.69578 |

---

<a id='query-23'></a>
### 23. Lock-query (уже с utid):
<details><summary>Показать SQL</summary>

```sql
SELECT
  t.name AS thread_name,
  s.name AS lock_slice,
  COUNT(*) AS count,
  ROUND(SUM(s.dur) / 1e6, 2) AS total_wait_ms
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t ON tt.utid = t.utid
WHERE s.name LIKE '%locked%'
   OR s.name LIKE '%contention%'
   OR s.name LIKE '%waiting for%'
GROUP BY thread_name, lock_slice
ORDER BY total_wait_ms DESC
LIMIT 20;
```
</details>

*Время выполнения:* **0.07 сек** | *Строк:* **20**

| thread_name | lock_slice | count | total_wait_ms |
| --- | --- | --- | --- |
| presentFence | waiting for presentFence 708 | 123 | 3999.23 |
| presentFence | waiting for presentFence 706 | 124 | 3990.95 |
| presentFence | waiting for presentFence 707 | 124 | 3964.56 |
| binder:18983_2 | releaseBufferCallbackLocked | 420 | 58.77 |
| binder:18983_4 | releaseBufferCallbackLocked | 324 | 50.53 |
| GPU completion | waiting for GPU completion 66 | 1 | 39.64 |
| GPU completion | waiting for GPU completion 157 | 1 | 39.25 |
| SDLThread | applyGrallocMetadataLocked | 372 | 39.11 |
| GPU completion | waiting for GPU completion 87 | 1 | 39.06 |
| GPU completion | waiting for GPU completion 94 | 1 | 38.95 |
| GPU completion | waiting for GPU completion 156 | 1 | 38.88 |
| GPU completion | waiting for GPU completion 75 | 1 | 38.88 |
| GPU completion | waiting for GPU completion 192 | 1 | 38.85 |
| GPU completion | waiting for GPU completion 161 | 1 | 38.83 |
| GPU completion | waiting for GPU completion 22 | 1 | 38.77 |
| GPU completion | waiting for GPU completion 187 | 1 | 38.73 |
| GPU completion | waiting for GPU completion 222 | 1 | 38.69 |
| GPU completion | waiting for GPU completion 59 | 1 | 38.68 |
| GPU completion | waiting for GPU completion 345 | 1 | 38.61 |
| GPU completion | waiting for GPU completion 233 | 1 | 38.59 |

---

<a id='query-24'></a>
### 24. 00. Sanity Check (stats & errors)
<details><summary>Показать SQL</summary>

```sql
SELECT name, idx, severity, source, value FROM stats WHERE value > 0 ORDER BY severity, name;
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **124**

| name | idx | severity | source | value |
| --- | --- | --- | --- | --- |
| trace_sorter_negative_timestamp_dropped |  | error | trace | 1 |
| clock_sync_cache_miss |  | info | analysis | 39 |
| compact_sched_switch_skipped |  | info | analysis | 8 |
| compact_sched_waking_skipped |  | info | analysis | 117 |
| ftrace_cpu_bytes_begin | 0 | info | trace | 508 |
| ftrace_cpu_bytes_begin | 1 | info | trace | 252 |
| ftrace_cpu_bytes_begin | 2 | info | trace | 1076 |
| ftrace_cpu_bytes_begin | 3 | info | trace | 928 |
| ftrace_cpu_bytes_begin | 4 | info | trace | 320 |
| ftrace_cpu_bytes_begin | 5 | info | trace | 536 |
| ftrace_cpu_bytes_begin | 6 | info | trace | 228 |
| ftrace_cpu_bytes_delta | 0 | info | trace | 329896 |
| ftrace_cpu_bytes_delta | 1 | info | trace | 322668 |
| ftrace_cpu_bytes_delta | 2 | info | trace | 319544 |
| ftrace_cpu_bytes_delta | 3 | info | trace | 317992 |
| ftrace_cpu_bytes_delta | 4 | info | trace | 320660 |
| ftrace_cpu_bytes_delta | 5 | info | trace | 321684 |
| ftrace_cpu_bytes_delta | 6 | info | trace | 315768 |
| ftrace_cpu_bytes_delta | 7 | info | trace | 322248 |
| ftrace_cpu_bytes_end | 0 | info | trace | 330404 |
| ftrace_cpu_bytes_end | 1 | info | trace | 322920 |
| ftrace_cpu_bytes_end | 2 | info | trace | 320620 |
| ftrace_cpu_bytes_end | 3 | info | trace | 318920 |
| ftrace_cpu_bytes_end | 4 | info | trace | 320980 |
| ftrace_cpu_bytes_end | 5 | info | trace | 322220 |
| ftrace_cpu_bytes_end | 6 | info | trace | 315996 |
| ftrace_cpu_bytes_end | 7 | info | trace | 322248 |
| ftrace_cpu_entries_begin | 0 | info | trace | 10 |
| ftrace_cpu_entries_begin | 1 | info | trace | 5 |
| ftrace_cpu_entries_begin | 2 | info | trace | 17 |
| ftrace_cpu_entries_begin | 3 | info | trace | 18 |
| ftrace_cpu_entries_begin | 4 | info | trace | 6 |
| ftrace_cpu_entries_begin | 5 | info | trace | 12 |
| ftrace_cpu_entries_begin | 6 | info | trace | 5 |
| ftrace_cpu_entries_delta | 0 | info | trace | 10 |
| ftrace_cpu_entries_delta | 4 | info | trace | 3 |
| ftrace_cpu_entries_end | 0 | info | trace | 20 |
| ftrace_cpu_entries_end | 1 | info | trace | 5 |
| ftrace_cpu_entries_end | 2 | info | trace | 10 |
| ftrace_cpu_entries_end | 3 | info | trace | 4 |
| ftrace_cpu_entries_end | 4 | info | trace | 9 |
| ftrace_cpu_entries_end | 5 | info | trace | 11 |
| ftrace_cpu_entries_end | 6 | info | trace | 4 |
| ftrace_cpu_now_ts_begin | 0 | info | trace | 1769209898333000 |
| ftrace_cpu_now_ts_begin | 1 | info | trace | 1769209898395000 |
| ftrace_cpu_now_ts_begin | 2 | info | trace | 1769209898444000 |
| ftrace_cpu_now_ts_begin | 3 | info | trace | 1769209898491000 |
| ftrace_cpu_now_ts_begin | 4 | info | trace | 1769209898536000 |
| ftrace_cpu_now_ts_begin | 5 | info | trace | 1769209898581000 |
| ftrace_cpu_now_ts_begin | 6 | info | trace | 1769209898627000 |
| ftrace_cpu_now_ts_begin | 7 | info | trace | 1769209898674000 |
| ftrace_cpu_now_ts_end | 0 | info | trace | 1769229190645000 |
| ftrace_cpu_now_ts_end | 1 | info | trace | 1769229190752000 |
| ftrace_cpu_now_ts_end | 2 | info | trace | 1769229190801000 |
| ftrace_cpu_now_ts_end | 3 | info | trace | 1769229190848000 |
| ftrace_cpu_now_ts_end | 4 | info | trace | 1769229190895000 |
| ftrace_cpu_now_ts_end | 5 | info | trace | 1769229190949000 |
| ftrace_cpu_now_ts_end | 6 | info | trace | 1769229190996000 |
| ftrace_cpu_now_ts_end | 7 | info | trace | 1769229191042000 |
| ftrace_cpu_oldest_event_ts_begin | 0 | info | trace | 1769209898004000 |
| ftrace_cpu_oldest_event_ts_begin | 1 | info | trace | 1769209897750000 |
| ftrace_cpu_oldest_event_ts_begin | 2 | info | trace | 1769209898042000 |
| ftrace_cpu_oldest_event_ts_begin | 3 | info | trace | 1769209897707000 |
| ftrace_cpu_oldest_event_ts_begin | 4 | info | trace | 1769209897967000 |
| ftrace_cpu_oldest_event_ts_begin | 5 | info | trace | 1769209897943000 |
| ftrace_cpu_oldest_event_ts_begin | 6 | info | trace | 1769209898031000 |
| ftrace_cpu_oldest_event_ts_begin | 7 | info | trace | 1769146630619000 |
| ftrace_cpu_oldest_event_ts_end | 0 | info | trace | 1769229186830000 |
| ftrace_cpu_oldest_event_ts_end | 1 | info | trace | 1769229189457000 |
| ftrace_cpu_oldest_event_ts_end | 2 | info | trace | 1769229179405000 |
| ftrace_cpu_oldest_event_ts_end | 3 | info | trace | 1769229189958000 |
| ftrace_cpu_oldest_event_ts_end | 4 | info | trace | 1769229181751000 |
| ftrace_cpu_oldest_event_ts_end | 5 | info | trace | 1769229175323000 |
| ftrace_cpu_oldest_event_ts_end | 6 | info | trace | 1769229171165000 |
| ftrace_cpu_oldest_event_ts_end | 7 | info | trace | 1769229151080000 |
| ftrace_cpu_read_events_delta | 0 | info | trace | 210702 |
| ftrace_cpu_read_events_delta | 1 | info | trace | 228916 |
| ftrace_cpu_read_events_delta | 2 | info | trace | 232028 |
| ftrace_cpu_read_events_delta | 3 | info | trace | 216025 |
| ftrace_cpu_read_events_delta | 4 | info | trace | 159385 |
| ftrace_cpu_read_events_delta | 5 | info | trace | 157128 |
| ftrace_cpu_read_events_delta | 6 | info | trace | 76506 |
| ftrace_cpu_read_events_delta | 7 | info | trace | 21425 |
| ftrace_cpu_read_events_end | 0 | info | trace | 210702 |
| ftrace_cpu_read_events_end | 1 | info | trace | 228916 |
| ftrace_cpu_read_events_end | 2 | info | trace | 232028 |
| ftrace_cpu_read_events_end | 3 | info | trace | 216025 |
| ftrace_cpu_read_events_end | 4 | info | trace | 159385 |
| ftrace_cpu_read_events_end | 5 | info | trace | 157128 |
| ftrace_cpu_read_events_end | 6 | info | trace | 76506 |
| ftrace_cpu_read_events_end | 7 | info | trace | 21425 |
| ftrace_setup_errors |  | info | trace | 39 |
| graphics_frame_event_parser_errors |  | info | analysis | 381 |
| guess_trace_type_duration_ns |  | info | analysis | 101200 |
| interned_data_tokenizer_errors |  | info | analysis | 62156 |
| parse_trace_duration_ns |  | info | analysis | 349803900 |
| traced_buf_buffer_size | 0 | info | trace | 134217728 |
| traced_buf_buffer_size | 1 | info | trace | 33554432 |
| traced_buf_bytes_read | 0 | info | trace | 24973312 |
| traced_buf_bytes_read | 1 | info | trace | 737280 |

*(Показаны первые 100 из 124 строк. Полный набор данных: `sql_results/24_00. Sanity Check _stats _ errors_.csv`)*


---

<a id='query-25'></a>
### 25. 00. Package profileable check
<details><summary>Показать SQL</summary>

```sql
SELECT * FROM package_list WHERE package_name = 'com.libertyrecomp';
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **1**

| id | package_name | uid | debuggable | profileable_from_shell | version_code |
| --- | --- | --- | --- | --- | --- |
| 0 | com.libertyrecomp | 10136 | 1 | 1 | 18 |

---

<a id='query-26'></a>
### 26. 00. Ftrace setup errors
<details><summary>Показать SQL</summary>

```sql
SELECT * FROM stats WHERE name GLOB 'ftrace_setup*' OR name GLOB '*ftrace*error*';
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **4**

| id | name | key | idx | severity | source | value | description | machine_id | trace_id |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 35 | ftrace_setup_errors | 53 |  | info | trace | 39 | One or more atrace/ftrace categories were not found or failed to enable. See ftrace_setup_errors in the metadata table for details. | 0 | 0 |
| 28 | ftrace_bundle_tokenizer_errors | 23 |  | error | analysis | 0 |  | 0 | 0 |
| 36 | ftrace_abi_errors_skipped_zero_data_length | 54 |  | info | analysis | 0 |  | 0 | 0 |
| 37 | ftrace_generic_descriptor_errors | 55 |  | error | analysis | 0 | The config is setting denser_generic_event_encoding, but there are issues with parsing or matching up the in-trace proto descriptors. | 0 | 0 |

---

<a id='query-27'></a>
### 27. 01. Ритм кадров (FPS и задержки) по маркерам Rec: FinishPresent
<details><summary>Показать SQL</summary>

```sql
INCLUDE PERFETTO MODULE slices.with_context;

WITH f AS (
  SELECT ts, LEAD(ts) OVER (ORDER BY ts) - ts AS iv
  FROM thread_slice
  WHERE process_name GLOB 'com.libertyrecomp*' AND name = 'Rec: FinishPresent'
),
g AS (
  SELECT iv, ROW_NUMBER() OVER (ORDER BY iv) rn, COUNT(*) OVER () n
  FROM f
  WHERE iv IS NOT NULL
)
SELECT MAX(n) AS total_frames,
       ROUND(1e9 / AVG(iv), 1)                                   AS avg_fps,
       ROUND(MIN(CASE WHEN rn >= 0.50 * n THEN iv END) / 1e6, 2)  AS p50_ms,
       ROUND(MIN(CASE WHEN rn >= 0.99 * n THEN iv END) / 1e6, 2)  AS p99_ms,
       ROUND(MAX(iv) / 1e6, 2)                                   AS max_ms,
       ROUND(1e9 / AVG(CASE WHEN rn > 0.99 * n THEN iv END), 1)   AS fps_1pct_low,
       SUM(iv > 20e6)                                            AS frames_over_20ms,
       SUM(iv > 33.4e6)                                          AS frames_over_33ms
FROM g;
```
</details>

*Время выполнения:* **0.01 сек** | *Строк:* **1**

| total_frames | avg_fps | p50_ms | p99_ms | max_ms | fps_1pct_low | frames_over_20ms | frames_over_33ms |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 371 | 19.3 | 50.65 | 73.67 | 79.73 | 13.0 | 371 | 371 |

---

<a id='query-28'></a>
### 28. 02. Маркеры профилирования игры: Rec: / RW: / RG: (Backpressure)
<details><summary>Показать SQL</summary>

```sql
INCLUDE PERFETTO MODULE slices.with_context;

WITH s AS (
  SELECT thread_name, name, dur,
         ROW_NUMBER() OVER (PARTITION BY thread_name, name ORDER BY dur) AS rn,
         COUNT(*)     OVER (PARTITION BY thread_name, name)              AS n
  FROM thread_slice
  WHERE process_name GLOB 'com.libertyrecomp*'
    AND (name GLOB 'Rec: *' OR name GLOB 'RW: *' OR name GLOB 'RG: *')
    AND dur >= 0
)
SELECT thread_name, name, MAX(n) AS cnt,
       ROUND(SUM(dur) / 1e6, 2) AS total_ms,
       ROUND(AVG(dur) / 1e6, 3) AS avg_ms,
       ROUND(MIN(CASE WHEN rn >= 0.50 * n THEN dur END) / 1e6, 3) AS p50_ms,
       ROUND(MIN(CASE WHEN rn >= 0.90 * n THEN dur END) / 1e6, 3) AS p90_ms,
       ROUND(MAX(dur) / 1e6, 3) AS max_ms
FROM s
GROUP BY thread_name, name
ORDER BY total_ms DESC;
```
</details>

*Время выполнения:* **0.05 сек** | *Строк:* **4**

| thread_name | name | cnt | total_ms | avg_ms | p50_ms | p90_ms | max_ms |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RenderGuest | Rec: FinishPresent | 372 | 340.71 | 0.916 | 0.915 | 1.129 | 1.711 |
| RenderGuest | Rec: PublishFrame | 372 | 292.45 | 0.786 | 0.792 | 0.978 | 1.361 |
| RenderGuest | RW: wait recorder free | 372 | 1.96 | 0.005 | 0.005 | 0.006 | 0.104 |
| RenderGuest | Rec: BeginModernShaderFrame | 372 | 1.03 | 0.003 | 0.003 | 0.003 | 0.005 |

---

<a id='query-29'></a>
### 29. 03. Фактические частоты CPU по кластерам (Little / Big / Prime)
<details><summary>Показать SQL</summary>

```sql
INCLUDE PERFETTO MODULE linux.cpu.frequency;

SELECT cpu,
       CASE WHEN cpu <= 3 THEN 'little' WHEN cpu <= 6 THEN 'big' ELSE 'prime' END AS cluster,
       COUNT(*) AS changes,
       MIN(freq) / 1000 AS min_mhz,
       MAX(freq) / 1000 AS max_mhz,
       ROUND(SUM(freq * dur) * 1.0 / SUM(dur) / 1000, 0) AS time_weighted_mhz
FROM cpu_frequency_counters
WHERE dur > 0
GROUP BY cpu
ORDER BY cpu;
```
</details>

*Время выполнения:* **0.29 сек** | *Строк:* **8**

| cpu | cluster | changes | min_mhz | max_mhz | time_weighted_mhz |
| --- | --- | --- | --- | --- | --- |
| 0 | little | 3605 | 499 | 1804 | 1602.0 |
| 1 | little | 3605 | 499 | 1804 | 1602.0 |
| 2 | little | 3605 | 499 | 1804 | 1602.0 |
| 3 | little | 3605 | 499 | 1804 | 1602.0 |
| 4 | big | 4028 | 710 | 2419 | 1796.0 |
| 5 | big | 4028 | 710 | 2419 | 1796.0 |
| 6 | big | 4028 | 710 | 2419 | 1796.0 |
| 7 | prime | 605 | 844 | 2841 | 2191.0 |

---

<a id='query-30'></a>
### 30. 04. Троттлинг: падения потолка частоты CPU (power/cpu_frequency_limits)
<details><summary>Показать SQL</summary>

```sql
WITH lim AS (
  SELECT c.ts, t.name AS track, t.type AS track_type, c.value,
         c.value - LAG(c.value) OVER (PARTITION BY c.track_id ORDER BY c.ts) AS delta
  FROM counter c JOIN track t ON c.track_id = t.id
  WHERE t.name GLOB '*Max Freq*' OR t.name GLOB '*max_freq*' OR t.type GLOB '*max_freq*'
     OR t.type GLOB '*frequency_limit*'
)
SELECT ts, track, track_type, CAST(value AS INT) AS max_khz, CAST(delta AS INT) AS delta_khz
FROM lim
WHERE delta IS NULL OR delta <> 0
ORDER BY ts;
```
</details>

*Время выполнения:* **0.46 сек** | *Строк:* **233**

| ts | track | track_type | max_khz | delta_khz |
| --- | --- | --- | --- | --- |
| 1769210308674435 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 |  |
| 1769210308674435 | Cpu 4 Min Freq Limit | cpu_min_frequency_limit | 710400 |  |
| 1769210361175008 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769210412911362 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769210580671362 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769210636831622 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769210692705945 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769210852873653 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769210908955216 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769210959252820 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769211063195632 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769211116824903 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769211172759226 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769211377103861 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769211428725164 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769211484965268 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769211536159591 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769211586655528 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769211637041466 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769211794971049 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769211845290268 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769211896085007 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769212004950788 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769212060818392 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769212115466830 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769212172725007 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769212223489955 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769212276883913 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769212388678913 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769212444713132 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769212500877819 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2112000 | -230400 |
| 1769212556697403 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | 115200 |
| 1769212612684590 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769212672638548 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769213204856048 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769213260707507 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2112000 | -115200 |
| 1769213316749486 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | 115200 |
| 1769213372684277 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769213428715111 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769213540663079 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769213596951152 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769213652886934 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769213869183913 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769213924707923 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769213978457246 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2112000 | -230400 |
| 1769214028813548 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | 115200 |
| 1769214084952767 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769214140504121 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2112000 | -230400 |
| 1769214193432246 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | 115200 |
| 1769214244677923 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769214295380267 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769214348891308 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769214401063704 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769214452860527 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769214508877194 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769214565043235 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769214619302819 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769215212758756 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769215268867402 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769215324721725 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769215700779329 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769215752884224 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769215804849901 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769215860664485 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769215916673183 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769215972717089 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769216025818391 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769216076755474 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769216130358703 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769216292931672 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769216348944120 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769216405417401 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769216516960630 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769216573226359 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769216628893755 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2112000 | -230400 |
| 1769216685229901 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | 115200 |
| 1769216735584537 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769216788802089 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769216891959068 | Cpu 7 Max Freq Limit | cpu_max_frequency_limit | 2496000 |  |
| 1769216891959068 | Cpu 7 Min Freq Limit | cpu_min_frequency_limit | 844800 |  |
| 1769216940993130 | Cpu 7 Max Freq Limit | cpu_max_frequency_limit | 2592000 | 96000 |
| 1769216992338078 | Cpu 7 Max Freq Limit | cpu_max_frequency_limit | 2688000 | 96000 |
| 1769217045071672 | Cpu 7 Max Freq Limit | cpu_max_frequency_limit | 2496000 | -192000 |
| 1769217100467349 | Cpu 7 Max Freq Limit | cpu_max_frequency_limit | 2380800 | -115200 |
| 1769217308919328 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769217364671203 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2112000 | -115200 |
| 1769217420857036 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | 115200 |
| 1769217476860422 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769217533140630 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2112000 | -230400 |
| 1769217588672297 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | 115200 |
| 1769217644896828 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769217694914692 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769217748748390 | Cpu 7 Max Freq Limit | cpu_max_frequency_limit | 2496000 | 115200 |
| 1769217803161776 | Cpu 7 Max Freq Limit | cpu_max_frequency_limit | 2380800 | -115200 |
| 1769217914613546 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769217965343078 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2342400 | 115200 |
| 1769218016097609 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2419200 | 76800 |
| 1769218068680317 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | -192000 |
| 1769218124712713 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2112000 | -115200 |
| 1769218236673807 | Cpu 4 Max Freq Limit | cpu_max_frequency_limit | 2227200 | 115200 |

*(Показаны первые 100 из 233 строк. Полный набор данных: `sql_results/30_04. Троттлинг_ падения потолка частоты CPU _power_cpu_freque.csv`)*


---

<a id='query-31'></a>
### 31. 05. Причина падения частоты: температура и cdev за 1 сек до троттлинга
<details><summary>Показать SQL</summary>

```sql
WITH drops AS MATERIALIZED (
  SELECT c.ts, t.name AS lim_track, c.value,
         c.value - LAG(c.value) OVER (PARTITION BY c.track_id ORDER BY c.ts) AS delta
  FROM counter c JOIN track t ON c.track_id = t.id
  WHERE t.name GLOB '*Max Freq*' OR t.type GLOB '*max_freq*' OR t.type GLOB '*frequency_limit*'
),
th AS MATERIALIZED (
  SELECT c.ts, t.name AS th_track, c.value
  FROM counter c JOIN track t ON c.track_id = t.id
  WHERE t.name GLOB '*Temperature*' OR t.name GLOB '*Cooling*' OR t.name GLOB '*cdev*'
     OR t.type GLOB '*thermal*' OR t.type GLOB '*cooling*'
)
SELECT d.ts AS drop_ts, d.lim_track, CAST(d.value AS INT) AS new_max_khz,
       (SELECT th.th_track FROM th WHERE th.ts <= d.ts AND th.ts >= d.ts - 1e9 ORDER BY th.ts DESC LIMIT 1) AS th_track,
       (SELECT th.value FROM th WHERE th.ts <= d.ts AND th.ts >= d.ts - 1e9 ORDER BY th.ts DESC LIMIT 1) AS th_value,
       (SELECT ROUND((d.ts - th.ts) / 1e6, 1) FROM th WHERE th.ts <= d.ts AND th.ts >= d.ts - 1e9 ORDER BY th.ts DESC LIMIT 1) AS ms_before
FROM drops d
WHERE d.delta < 0
ORDER BY d.ts;
```
</details>

*Время выполнения:* **0.87 сек** | *Строк:* **78**

| drop_ts | lim_track | new_max_khz | th_track | th_value | ms_before |
| --- | --- | --- | --- | --- | --- |
| 1769210580671362 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769210852873653 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769211063195632 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769211377103861 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769211536159591 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769211794971049 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769212004950788 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769212172725007 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769212388678913 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769212500877819 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769213204856048 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769213260707507 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769213540663079 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769213869183913 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769213978457246 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769214140504121 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769214348891308 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769214508877194 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769215212758756 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769215700779329 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769215860664485 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769216025818391 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769216292931672 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769216516960630 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769216628893755 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769217045071672 | Cpu 7 Max Freq Limit | 2496000 |  |  |  |
| 1769217100467349 | Cpu 7 Max Freq Limit | 2380800 |  |  |  |
| 1769217308919328 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769217364671203 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769217533140630 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769217803161776 | Cpu 7 Max Freq Limit | 2380800 |  |  |  |
| 1769217914613546 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769218068680317 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769218124712713 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769218292811567 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769218460864692 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769218668832973 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769218828918702 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769218935478129 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769219154180421 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769219516842608 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769219732659483 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769220276668650 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769220388848702 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769221540702191 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769221596728076 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769221812679691 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769222420978545 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769222532777503 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769223372863284 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769223635448909 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769224556904169 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769224863328179 | Cpu 7 Max Freq Limit | 2688000 |  |  |  |
| 1769224916193700 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769225132641460 | Cpu 7 Max Freq Limit | 2688000 |  |  |  |
| 1769226149113960 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769226308697033 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769226474839585 | Cpu 7 Max Freq Limit | 2688000 |  |  |  |
| 1769226524739429 | Cpu 7 Max Freq Limit | 2496000 |  |  |  |
| 1769226579103283 | Cpu 7 Max Freq Limit | 2380800 |  |  |  |
| 1769226736792345 | Cpu 7 Max Freq Limit | 2380800 |  |  |  |
| 1769226844922762 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769227116720939 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769227168701355 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769227388728907 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769227444985626 | Cpu 7 Max Freq Limit | 2688000 |  |  |  |
| 1769227700695470 | Cpu 7 Max Freq Limit | 2688000 |  |  |  |
| 1769227752439584 | Cpu 7 Max Freq Limit | 2496000 |  |  |  |
| 1769227860683126 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769228020840991 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769228070871407 | Cpu 4 Max Freq Limit | 2112000 |  |  |  |
| 1769228178693126 | Cpu 7 Max Freq Limit | 2688000 |  |  |  |
| 1769228229332084 | Cpu 7 Max Freq Limit | 2496000 |  |  |  |
| 1769228280352657 | Cpu 7 Max Freq Limit | 2380800 |  |  |  |
| 1769228436683855 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769228907140938 | Cpu 7 Max Freq Limit | 2688000 |  |  |  |
| 1769228961984271 | Cpu 4 Max Freq Limit | 2227200 |  |  |  |
| 1769229066486459 | Cpu 7 Max Freq Limit | 2688000 |  |  |  |

---

<a id='query-32'></a>
### 32. 06. Размещение потоков игры по ядрам и миграции между кластерами
<details><summary>Показать SQL</summary>

```sql
WITH s AS (
  SELECT s.utid, s.cpu, s.dur, s.ts,
         LAG(s.cpu) OVER (PARTITION BY s.utid ORDER BY s.ts) AS prev_cpu
  FROM sched s JOIN thread t USING(utid) JOIN process p USING(upid)
  WHERE p.name GLOB 'com.libertyrecomp*'
)
SELECT t.name AS thread,
  ROUND(SUM(CASE WHEN cpu <= 3 THEN dur END) / 1e6, 1)            AS little_ms,
  ROUND(SUM(CASE WHEN cpu BETWEEN 4 AND 6 THEN dur END) / 1e6, 1) AS big_ms,
  ROUND(SUM(CASE WHEN cpu = 7 THEN dur END) / 1e6, 1)             AS prime_ms,
  ROUND(SUM(dur) / 1e6, 1)                                        AS total_ms,
  SUM(prev_cpu IS NOT NULL AND prev_cpu <> cpu)                 AS migrations,
  SUM(prev_cpu IS NOT NULL AND
      (CASE WHEN prev_cpu <= 3 THEN 0 WHEN prev_cpu <= 6 THEN 1 ELSE 2 END) <>
      (CASE WHEN cpu <= 3 THEN 0 WHEN cpu <= 6 THEN 1 ELSE 2 END)) AS cross_cluster
FROM s JOIN thread t USING(utid)
GROUP BY s.utid
ORDER BY total_ms DESC
LIMIT 30;
```
</details>

*Время выполнения:* **1.30 сек** | *Строк:* **30**

| thread | little_ms | big_ms | prime_ms | total_ms | migrations | cross_cluster |
| --- | --- | --- | --- | --- | --- | --- |
| RenderGuest |  | 2087.3 | 16901.4 | 18988.7 | 134 | 134 |
| RageStream | 10328.5 |  |  | 10328.5 | 3600 | 0 |
| AudioGuest |  | 7233.3 |  | 7233.3 | 420 | 0 |
| Main XThread (F |  | 6446.1 | 85.6 | 6531.7 | 390 | 70 |
| SDLThread | 1934.5 |  |  | 1934.5 | 6858 | 0 |
| Rage_299490 | 1304.2 |  |  | 1304.2 | 1135 | 0 |
| GtaCmdRetirer | 1151.1 |  |  | 1151.1 | 427 | 0 |
| RageJobWork | 196.7 | 849.0 |  | 1045.7 | 3077 | 756 |
| RageJobWork | 175.2 | 868.6 |  | 1043.9 | 3032 | 706 |
| RageJobWork | 192.0 | 846.8 |  | 1038.8 | 3012 | 722 |
| Rage_556188 | 987.4 |  |  | 987.4 | 7142 | 0 |
| SDLActivity | 783.4 |  |  | 783.4 | 8525 | 0 |
| RagePhysics | 22.5 | 642.8 |  | 665.3 | 721 | 60 |
| RagePhysics | 17.1 | 616.1 |  | 633.1 | 726 | 50 |
| RagePhysics | 12.7 | 604.5 |  | 617.2 | 723 | 40 |
| Rage_3A05E8 | 533.9 |  |  | 533.9 | 936 | 0 |
| SDLActivity | 419.2 |  |  | 419.2 | 806 | 0 |
| Audio Worker (F |  | 267.1 |  | 267.1 | 1605 | 0 |
| Rage_1BA088 | 85.8 | 91.9 |  | 177.7 | 493 | 102 |
| binder:18983_2 | 138.1 |  |  | 138.1 | 217 | 0 |
| binder:18983_4 | 111.7 |  |  | 111.7 | 147 | 0 |
| SDLThread | 101.1 |  |  | 101.1 | 964 | 0 |
| Rage_5FBB68 | 86.7 |  |  | 86.7 | 655 | 0 |
| AudioTrack |  | 84.6 | 0.3 | 84.9 | 365 | 6 |
| GPU completion | 71.2 |  |  | 71.2 | 610 | 0 |
| SDLActivity | 66.6 |  |  | 66.6 | 107 | 0 |
| RageFileIO | 61.3 |  |  | 61.3 | 495 | 0 |
| SDLThread | 45.5 |  |  | 45.5 | 471 | 0 |
| RageJobWork | 44.3 | 1.2 |  | 45.4 | 485 | 12 |
| RageFileIO | 45.4 |  |  | 45.4 | 497 | 0 |

---

<a id='query-33'></a>
### 33. 07. Время в очереди на CPU (Runnable / Preempted) по игровым потокам
<details><summary>Показать SQL</summary>

```sql
WITH r AS (
  SELECT t.name AS thread, ts.state, ts.dur,
         ROW_NUMBER() OVER (PARTITION BY ts.utid ORDER BY ts.dur) rn,
         COUNT(*)     OVER (PARTITION BY ts.utid)                 n
  FROM thread_state ts JOIN thread t USING(utid) JOIN process p USING(upid)
  WHERE p.name GLOB 'com.libertyrecomp*' AND ts.state IN ('R', 'R+')
)
SELECT thread, MAX(n) AS cnt, SUM(state = 'R+') AS preempted_cnt,
       ROUND(SUM(dur) / 1e6, 2) AS total_wait_cpu_ms,
       ROUND(MIN(CASE WHEN rn >= 0.99 * n THEN dur END) / 1e6, 3) AS p99_wait_ms,
       ROUND(MAX(dur) / 1e6, 3) AS max_wait_ms
FROM r
GROUP BY thread
ORDER BY total_wait_cpu_ms DESC
LIMIT 30;
```
</details>

*Время выполнения:* **1.07 сек** | *Строк:* **30**

| thread | cnt | preempted_cnt | total_wait_cpu_ms | p99_wait_ms | max_wait_ms |
| --- | --- | --- | --- | --- | --- |
| RageJobWork | 14988 | 323 | 4225.76 | 0.909 | 11.874 |
| Rage_556188 | 17604 | 165 | 2287.33 | 1.966 | 9.823 |
| SDLThread | 17572 | 1225 | 2135.94 | 0.058 | 8.694 |
| RagePhysics | 2004 | 141 | 1832.44 | 3.782 | 11.432 |
| SDLActivity | 19529 | 254 | 1678.57 | 0.679 | 4.276 |
| RageStream | 12185 | 10 | 979.74 | 0.729 | 6.454 |
| Audio Worker (F | 15638 | 5 | 663.09 | 0.792 | 10.303 |
| Rage_299490 | 2425 | 85 | 320.28 | 1.221 | 4.495 |
| RenderGuest | 665 | 6 | 300.06 | 0.021 | 14.725 |
| Rage_3A05E8 | 1772 | 7 | 249.74 | 1.236 | 3.189 |
| GtaCmdRetirer | 1136 | 14 | 211.46 | 1.32 | 5.889 |
| AudioGuest | 2675 | 702 | 206.55 | 1.73 | 8.938 |
| Rage_9FF398 | 974 | 18 | 166.52 | 0.76 | 2.629 |
| RageFileIO | 1013 | 37 | 161.17 | 0.586 | 1.803 |
| Rage_5FBB68 | 1391 | 8 | 160.24 | 1.316 | 3.536 |
| Rage_1BA088 | 1242 | 1 | 121.86 | 1.468 | 4.881 |
| GPU completion | 932 | 54 | 114.77 | 0.81 | 1.303 |
| Rage_212F38 | 980 | 12 | 78.75 | 0.778 | 1.725 |
| Main XThread (F | 2311 | 5 | 68.68 | 0.302 | 3.267 |
| XMA Decoder (F8 | 961 | 0 | 57.21 | 1.0 | 8.13 |
| SDLAudioP15 | 969 | 2 | 50.82 | 1.189 | 4.041 |
| binder:18983_3 | 409 | 5 | 41.11 | 0.593 | 1.796 |
| AudioTrack | 1453 | 0 | 35.86 | 0.108 | 1.438 |
| binder:18983_2 | 366 | 68 | 33.06 | 0.687 | 1.986 |
| binder:18983_4 | 301 | 67 | 31.51 | 0.704 | 0.973 |
| m.libertyrecomp | 8 | 2 | 2.13 | 0.506 | 0.506 |
| UIMonitorThread | 7 | 0 | 0.57 | 0.275 | 0.275 |
| Jit thread pool | 3 | 0 | 0.56 | 0.207 | 0.207 |
| binder:18919_1 | 3 | 1 | 0.26 | 0.157 | 0.157 |
| Kernel Host Tas | 2 | 0 | 0.09 | 0.046 | 0.046 |

---

<a id='query-34'></a>
### 34. 08. Кто будит RenderGuest / GtaRecorder / GtaRenderWork (waker поток)
<details><summary>Показать SQL</summary>

```sql
SELECT t.name AS wakee, COALESCE(w.name, '<irq/unknown>') AS waker,
       COUNT(*) AS wakeups, SUM(ts.irq_context = 1) AS from_irq,
       ROUND(AVG(ts.dur) / 1e3, 1) AS avg_wake_to_run_us,
       ROUND(MAX(ts.dur) / 1e6, 3) AS max_wake_to_run_ms
FROM thread_state ts
JOIN thread t USING(utid)
LEFT JOIN thread w ON w.utid = ts.waker_utid
WHERE t.name IN ('RenderGuest', 'GtaRecorder', 'GtaRenderWork') AND ts.state = 'R'
GROUP BY t.name, waker
ORDER BY wakeups DESC
LIMIT 40;
```
</details>

*Время выполнения:* **0.27 сек** | *Строк:* **8**

| wakee | waker | wakeups | from_irq | avg_wake_to_run_us | max_wake_to_run_ms |
| --- | --- | --- | --- | --- | --- |
| RenderGuest | swapper | 371 | 371 | 475.5 | 14.725 |
| RenderGuest | <irq/unknown> | 126 |  | 39.2 | 0.282 |
| RenderGuest | RenderGuest | 101 | 0 | 1051.1 | 11.604 |
| RenderGuest | GtaCmdRetirer | 56 | 0 | 56.0 | 0.95 |
| RenderGuest | Main XThread (F | 54 | 0 | 99.5 | 4.649 |
| RenderGuest | SDLThread | 20 | 0 | 18.6 | 0.03 |
| RenderGuest | RageStream | 1 | 0 | 20.9 | 0.021 |
| RenderGuest | stack-unwinding | 1 | 1 | 3300.1 | 3.3 |

---

<a id='query-35'></a>
### 35. 09. Распределение снов (S-state) и подозрение на пересып (TimerSlack)
<details><summary>Показать SQL</summary>

```sql
SELECT t.name AS thread, COUNT(*) AS sleeps,
  SUM(ts.dur < 50e3)                       AS lt_50us,
  SUM(ts.dur >= 50e3  AND ts.dur < 500e3)  AS us50_500,
  SUM(ts.dur >= 500e3 AND ts.dur < 1e6)    AS us500_1ms,
  SUM(ts.dur >= 1e6   AND ts.dur < 2e6)    AS ms1_2,
  SUM(ts.dur >= 2e6)                       AS ge_2ms,
  ROUND(MAX(ts.dur) / 1e6, 2)              AS max_sleep_ms
FROM thread_state ts JOIN thread t USING(utid)
WHERE t.name IN ('RenderGuest', 'AudioGuest', 'RageStream', 'GtaRecorder') AND ts.state = 'S'
GROUP BY t.name
ORDER BY sleeps DESC;
```
</details>

*Время выполнения:* **0.44 сек** | *Строк:* **3**

| thread | sleeps | lt_50us | us50_500 | us500_1ms | ms1_2 | ge_2ms | max_sleep_ms |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RageStream | 9196 | 82 | 7877 | 38 | 62 | 1137 | 9.0 |
| AudioGuest | 2117 | 645 | 215 | 79 | 203 | 975 | 20.05 |
| RenderGuest | 889 | 389 | 415 | 17 | 5 | 63 | 5860.48 |

---

<a id='query-36'></a>
### 36. 10. Idle-состояния процессора (активность против глубокого сна)
<details><summary>Показать SQL</summary>

```sql
WITH e AS (
  SELECT t.cpu, c.ts, c.value,
         LEAD(c.ts) OVER (PARTITION BY t.cpu ORDER BY c.ts) - c.ts AS dur
  FROM counter c JOIN cpu_counter_track t ON c.track_id = t.id
  WHERE t.name = 'cpuidle' OR t.type GLOB '*idle*'
)
SELECT cpu,
       CASE WHEN value = 4294967295 THEN 'active' ELSE 'C' || CAST(value AS INT) END AS state,
       COUNT(*) AS n,
       ROUND(SUM(dur) / 1e6, 1) AS total_ms
FROM e
WHERE dur IS NOT NULL
GROUP BY cpu, state
ORDER BY cpu, total_ms DESC;
```
</details>

*Время выполнения:* **1.23 сек** | *Строк:* **20**

| cpu | state | n | total_ms |
| --- | --- | --- | --- |
| 0 | active | 26456 | 11233.5 |
| 0 | C0 | 26456 | 8055.3 |
| 1 | active | 29628 | 10975.5 |
| 1 | C0 | 29628 | 8316.2 |
| 2 | active | 27649 | 11039.8 |
| 2 | C0 | 27649 | 8243.0 |
| 3 | active | 27605 | 11030.8 |
| 3 | C0 | 27606 | 8260.0 |
| 4 | C0 | 23435 | 11835.4 |
| 4 | active | 23436 | 7445.8 |
| 4 | C1 | 2 | 9.2 |
| 5 | C0 | 23203 | 11667.6 |
| 5 | active | 23205 | 7620.3 |
| 5 | C1 | 1 | 4.4 |
| 6 | C0 | 6284 | 11621.1 |
| 6 | active | 6289 | 7613.2 |
| 6 | C1 | 6 | 47.8 |
| 7 | active | 1221 | 16996.5 |
| 7 | C0 | 1212 | 1932.8 |
| 7 | C1 | 9 | 319.6 |

---

<a id='query-37'></a>
### 37. 11. Частота GPU (Adreno devfreq / kgsl)
<details><summary>Показать SQL</summary>

```sql
SELECT t.name, t.type, COUNT(*) n, MIN(c.value) mn, MAX(c.value) mx
FROM counter c JOIN track t ON c.track_id = t.id
WHERE t.name GLOB '*kgsl*' OR t.name GLOB '*gpu*' OR t.name GLOB '*devfreq*' OR t.type GLOB '*devfreq*'
GROUP BY t.id;
```
</details>

*Время выполнения:* **0.43 сек** | *Строк:* **0**

*0 строк возвращено*


---

<a id='query-38'></a>
### 38. 12. Потребление батареи (ток разряда/заряда)
<details><summary>Показать SQL</summary>

```sql
SELECT t.name, COUNT(*) n, MIN(c.value) min_val, MAX(c.value) max_val, ROUND(AVG(c.value), 0) avg_val
FROM counter c JOIN counter_track t ON c.track_id = t.id
WHERE t.name GLOB 'batt.*'
GROUP BY t.name;
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **2**

| name | n | min_val | max_val | avg_val |
| --- | --- | --- | --- | --- |
| batt.capacity_pct | 20 | 100.0 | 100.0 | 100.0 |
| batt.current_ua | 20 | 417.0 | 581.0 | 484.0 |

---

<a id='query-39'></a>
### 39. 13. Горячие функции GtaRecorder (Callstack Sampling через linux.perf)
<details><summary>Показать SQL</summary>

```sql
WITH RECURSIVE smp AS (
  SELECT s.callsite_id AS cid FROM perf_sample s JOIN thread t USING(utid)
  WHERE t.name = 'GtaRecorder' AND s.callsite_id IS NOT NULL
),
anc(cid, leaf) AS (
  SELECT cid, 1 FROM smp
  UNION ALL
  SELECT c.parent_id, 0 FROM anc JOIN stack_profile_callsite c ON c.id = anc.cid
  WHERE c.parent_id IS NOT NULL
)
SELECT COALESCE(sym.name, f.name, '0x' || printf('%x', f.rel_pc)) AS func,
       m.name AS binary,
       SUM(anc.leaf) AS self_samples,
       COUNT(*) AS inclusive_samples
FROM anc JOIN stack_profile_callsite c ON c.id = anc.cid
JOIN stack_profile_frame f ON f.id = c.frame_id
JOIN stack_profile_mapping m ON m.id = f.mapping
LEFT JOIN stack_profile_symbol sym ON sym.id = f.symbol_set_id
GROUP BY func, binary
ORDER BY self_samples DESC
LIMIT 40;
```
</details>

*Время выполнения:* **0.02 сек** | *Строк:* **0**

*0 строк возвращено*


---

<a id='query-40'></a>
### 40. 14. Сэмплы функций во время тяжелых лагов (интервалы FinishPresent > 33.4 мс)
<details><summary>Показать SQL</summary>

```sql
INCLUDE PERFETTO MODULE slices.with_context;

WITH f AS (
  SELECT ts, LEAD(ts) OVER (ORDER BY ts) AS next_ts FROM thread_slice
  WHERE process_name GLOB 'com.libertyrecomp*' AND name = 'Rec: FinishPresent'
),
longf AS (
  SELECT ts, next_ts FROM f WHERE next_ts - ts > 33.4e6
)
SELECT t.name AS thread, COALESCE(sym.name, fr.name) AS leaf_func, m.name AS binary, COUNT(*) AS samples
FROM perf_sample s JOIN longf l ON s.ts >= l.ts AND s.ts < l.next_ts
JOIN thread t USING(utid)
JOIN stack_profile_callsite c ON c.id = s.callsite_id
JOIN stack_profile_frame fr ON fr.id = c.frame_id
JOIN stack_profile_mapping m ON m.id = fr.mapping
LEFT JOIN stack_profile_symbol sym ON sym.id = fr.symbol_set_id
GROUP BY thread, leaf_func, binary
ORDER BY samples DESC
LIMIT 40;
```
</details>

*Время выполнения:* **16.10 сек** | *Строк:* **40**

| thread | leaf_func | binary | samples |
| --- | --- | --- | --- |
| AudioGuest | __imp__sub_829321A0 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 385 |
| RageStream | sub_829134C8 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 381 |
| RenderGuest |  | /data/data/com.libertyrecomp/files/drivers/r6-4fd2fd30311a/libvulkan_freedreno.so | 266 |
| RageStream | sub_82914758 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 216 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem19CaptureTitleCommandEjPKvmPKNS2_13CaptureReplayE | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 212 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem21RecordNativeFrameTaskEP17VkCommandBuffer_TRKjS6_RKP9VkImage_TRKP13VkImageView_TjRKNSt6__ndk110shared_ptrIKNS2_21NativeTextureResourceEEERKNSG_IKNS1_19EnvironmentalDataV2EEERKbRKfRbSV_SS_S6_SS_b.resume | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 199 |
| AudioGuest | sub_82199BC8 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 194 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem21CaptureBufferResourceEj | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 190 |
| GtaCmdRetirer | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem13NativeCommand15RetireResourcesEv | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 187 |
| RenderGuest | __aarch64_cas2_acq | /apex/com.android.runtime/lib64/bionic/libc.so | 187 |
| RageStream | __imp__sub_82919810 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 164 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem24PrepareFrameTexturesTaskEP17VkCommandBuffer_Tbjbb.resume | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 159 |
| RageStream | sub_829055A8 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 158 |
| Main XThread (F | syscall | /apex/com.android.runtime/lib64/bionic/libc.so | 149 |
| RageJobWork | syscall | /apex/com.android.runtime/lib64/bionic/libc.so | 146 |
| RenderGuest | __memcpy_aarch64_simd | /apex/com.android.runtime/lib64/bionic/libc.so | 139 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem16SnapshotPipelineERKNS2_13NativeCommandEb | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 138 |
| AudioGuest | sub_82935BD0 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 130 |
| AudioGuest | sub_821997F8 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 121 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem16RenderWorkerLoopENS2_14WorkerLoopModeE | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 118 |
| RageStream | sub_82912FC0 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 116 |
| AudioGuest | sub_82936CE0 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 114 |
| Rage_556188 | syscall | /apex/com.android.runtime/lib64/bionic/libc.so | 102 |
| SDLThread | nanosleep | /apex/com.android.runtime/lib64/bionic/libc.so | 100 |
| RageStream | sub_82908D98 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 98 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem19BindCommonDrawStateEP17VkCommandBuffer_TRKNS2_13NativeCommandEP12VkPipeline_Tjjjj21VkSampleCountFlagBitsRNS2_20NativeFrameResourcesERKNS2_21NativeRenderingTargetE | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 92 |
| AudioGuest | sub_82934F00 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 87 |
| GtaCmdRetirer | _ZN3rex8graphics11gta4_native17NativeCommandPoolINS1_24Gta4NativeGraphicsSystem13NativeCommandEE11RetireBatchINSt6__ndk16vectorINS7_10unique_ptrIS4_NS5_7DeleterEEENS7_9allocatorISB_EEEEEEvRT_ | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 86 |
| RagePhysics | _ZNK3rex6thread18PosixConditionBase19SpinUntilEpochMovesEjjPKNSt6__ndk16atomicIbEEb | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexruntime.so | 85 |
| AudioGuest | sub_821B0310 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 79 |
| RenderGuest | _ZZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem24WriteWorkerRingConstantsERNS2_13NativeCommandEPKNS2_12NativeShaderES7_NSt6__ndk14spanIKhLm18446744073709551615EEESB_ENK3$_0clEjRKNS8_10shared_ptrIKNS1_20ConstantStateVersionEEESB_S7_mm | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 76 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem22RecordIndexedPrimitiveEP17VkCommandBuffer_TRKNS2_13NativeCommandEjjRKNS2_21NativeRenderingTargetERNS2_20NativeFrameResourcesE | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 75 |
| RenderGuest | _ZN3rex8graphics11gta4_native24Gta4NativeGraphicsSystem22ValidateAndCopyCommandEPKvmRNS2_13NativeCommandEPKNS2_13CaptureReplayE | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 75 |
| SDLThread | __ioctl | /apex/com.android.runtime/lib64/bionic/libc.so | 73 |
| RageStream | sub_829180A0 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 72 |
| RenderGuest | __aarch64_swp2_rel | /apex/com.android.runtime/lib64/bionic/libc.so | 70 |
| RagePhysics | sub_82443D40 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 66 |
| RenderGuest | XXH3_hashLong_64b_default | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/librexgpu-gta4-native.so | 66 |
| RageStream | __imp__sub_82A11940 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 65 |
| RageJobWork | sub_821D6260 | /data/app/~~pu3FLGO3vQMzJS9KIHsqKg==/com.libertyrecomp-n9mvymEzTmFeN1Eyh0beew==/lib/arm64/libmain.so | 63 |

---

<a id='query-41'></a>
### 41. 15. Логи игры, OPLUS и Thermal на едином таймлайне (android.log)
<details><summary>Показать SQL</summary>

```sql
SELECT l.ts, l.prio, l.tag, l.msg, t.name AS thread
FROM android_logs l LEFT JOIN thread t USING(utid)
WHERE l.tag GLOB '*LibertyRecomp*' OR l.tag GLOB '*hermal*' OR l.tag GLOB '*OPLUS*' OR l.tag GLOB '*oplus*'
ORDER BY l.ts
LIMIT 2000;
```
</details>

*Время выполнения:* **0.00 сек** | *Строк:* **0**

*0 строк возвращено*


---

**Весь анализ завершён за 32.53 секунд.**

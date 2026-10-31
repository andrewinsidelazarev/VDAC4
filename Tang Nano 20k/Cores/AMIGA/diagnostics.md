# Amiga black-screen investigation — 2026-10-29

## Confirmed reset fault and correction

Hardware report `diagnostics/20261029-204027Z.decoded.json` from `0.3-debug1`
shows top status `0003FC3C`: PLL and SDRAM ready, ROM ready, buttons `00`,
warm-reset count 255, core reset asserted. CPU reset_n is zero and the CPU step
count is zero. All 262144 ROM word-write pulses were generated. A second read
reproduced status `0003FC3C` and zero CPU steps.

The wrapper incorrectly used `!btn[0]` to hold reset. Tang's buttons are active
high; NanoMig maps S2/reset to pin 87, which is VDAC4 `btn[1]`, with a pull-down.
The corrected `0.3-debug2` build synchronizes `btn[1]`, asserts reset on high,
and enables the matching pull-downs in the Amiga constraints. Its diagnostic
tag is `414D4402`. The real generated reset block passes the regression for
idle-low boot, held/released S2, S1, ROM gating, software, keyboard and power
resets. At that stage hardware boot was still unverified; the subsequent debug4
result below confirms boot after also correcting the chipset return timing.

This explains why the CPU never started in the observed run. It does not yet
prove correct SDRAM readback or successful Kickstart execution after release.

The user reports a valid HDMI signal with a black image; F12 returns to Wild
Commander. Live LINK reads while Amiga is running confirm VERSION `05010001`,
ROM status `4`, length `00080000`, and transfer CRC `FC24AE0D`. These verify
receipt by the ROM loader, **not SDRAM contents or CPU execution**.

The diagnostic FPGA adds passive observation; ROM loading, CPU/chipset settings,
SD bridge, RGB888 output and ESP-only audio remain as in release 0.2. The release
0.2 artifacts are retained. Debug build identity is separate from the compatible
LINK ABI: byte address `60` returns `414D4401` for debug1 and `414D4402` for debug2.

## Readout

`python board_diag.py 192.168.1.110 --source 192.168.1.2` checks identity, reads all diagnostic words,
and saves raw and decoded JSON under `diagnostics/`. The script makes no writes.
`board_read.py` waits for the response to the exact requested register; ongoing
audio log lines cannot prematurely terminate a read.

All addresses below are hexadecimal **byte** addresses.

| Address | Contents |
| --- | --- |
| 60 | Diagnostic build identity (`414D4401` or `414D4402`) |
| 64 | bit 1 reset28, 2 PLL lock, 3 SDRAM ready, 4 ROM ready, 5 core reset, 6 soft reset, 7 keyboard reset, 9:8 button levels, 17:10 warm-reset count |
| 68 | VS falling-edge count in upper 16 bits, HS falling-edge count in lower 16 |
| 6C | Number of clocks with nonzero RGB at the HDMI input |
| 70 | CPU flags: bit 0 reset_n, 1 reset_out_n, 2 RW, 3 AS_n, 4 DTACK_n, 5 overlay, 6 phase sync; 10:7 CACR, 12:11 CPU state, 15:13 IPL_n, 16 LDS_n, 17 UDS_n, 18 RAM ready, 19 RAM request |
| 74 | CPU memory-state step count (CPU enable and state != internal) |
| 78 | Live CPU address, before SDRAM remapping |
| 7C | Live CPU read data in upper 16 bits, write data in lower 16 |
| 80 | Address at the last CPU memory-state step |
| 84 | Read/write data at that step |
| 88 | Chipset bus byte address |
| 8C | Chipset read/write data |
| 90–CC | First eight CPU memory-state steps: alternating address and read/write data |
| D0 | ROM write-pulse count; a full transfer produces 262144 |
| D4 | SDRAM byte address in bits 22:1, byte masks 24:23, write 25, select 26 |
| D8 | SDRAM read data in upper 16 bits, write data in lower 16 |
| DC | Live RGB888 in lower 24 bits |

The trace clears on external/core reset, then freezes after eight memory-state
steps. A subsequent CPU-generated reset preserves it. CPU read data is sampled
on the same enable that advances TG68K. The observer does not separately filter
the wrapper's `skip_fetch` signal, so skipped prefetch steps can also appear;
these are not proof of an SDRAM read. Match the initial vector addresses and
data before drawing conclusions. Registers are read sequentially: live
addresses, counters and status are not an atomic snapshot. The frozen initial
trace is suitable for matching addresses to ROM words.

The supplied 512 KiB ROM has a valid Amiga checksum and SHA-256
`8c8a0cf04f91b88eaf0c4f1126041987067e2286a8ee590bdbae447a8000c5ee`.
Use its actual first words when interpreting reset-vector reads; a successful
transfer CRC alone must not be reported as successful boot.

## Verification

`tests/run.py` checks the read-only debug address window as well as the existing
QSPI/ROM/CRC/HID/audio/SD behavior. `tests/run_debug.py` checks CPU reset/stall
gating, preservation of first completed reads while later accesses continue,
and clearing only on the external reset. `build.py fpga` gates a deployable
artifact on Gowin timing reports. Hardware readout remains necessary.

## Unreal-ng integration

The neighboring task's worktree is `E:\zx\unreal-ng\src\scratch\wt-vdac4`.
Its VDAC4 adapter currently targets Z80/SPI, native ESP application adapters,
and BL616 rendering for ROOMTEST. It does not simulate this Amiga FPGA or its
SDRAM, so its results do not establish whether TG68K reads Kickstart correctly.

## Logical boot simulation after the reset correction

`tests/run_boot.py --seconds 6` completed with the debug2 generated wrapper,
chipset, CPU wrapper and SDRAM controller. It uses NanoMig's checked-in GHDL
Verilog translation of TG68K and upstream SDRAM chip model. The simulation-only
CPU wrapper connects `skipFetch` and `berr`, omitted by the upstream VERILATOR
instantiation, to match the synthesized VHDL instantiation. FPGA sources and
the debug2 binary were not changed by this adapter.

Non-ROM SDRAM was initialized to deterministic nonzero data. The supplied ROM
was preloaded at its mapped SDRAM location. The trace matched reset-vector
words `1114 4EF9 00F8 00D2` and reached code at `00F800D2`. At six simulated
seconds, both core and CPU resets were released, SDRAM was ready, and the
observer had counted 32,450,979 memory-state steps and 298 video frames.

The saved RGB frame was visually inspected: it displays Kickstart 3.1 ROM
40.063, the rainbow check mark and disk-drive invitation on a purple background.
The report, source hashes, log and frame are archived under
`release/0.3-debug2/reports/boot-simulation/` and `diagnostics/boot-simulation/`.
This establishes a logical boot in this model, not successful board boot.
LINK ROM download, the synthesized VHDL CPU, PLLs, electrical SDRAM timing,
TMDS serialization, physical inputs, SD media and the audio DAC are outside
this boot simulation. Separate LINK and RGB888 tests remain documented above.

On October 29 the user was remote until morning. Both ESP network services
(ports 23 and 8268) timed out; no USB serial port was available. The board still
had debug1 at that point.

## Debug2 installation — October 30

After the user's power cycle, the board responded as TS-Config `01010120` with
ESP firmware `vdac4-0.1.105-heat2`. Debug2 `CORES/AMIGA.BIN` was copied to Tang
SD and verified by read-back SHA-256; the AMIGA entry in BL616's core cache was
then updated with the same confirmed hash:
`3165d5a869e8e6a05ad450c8b7abfd340fab7a602c9e11d22cd4a554768c8d80`.
Reports are in `deployment/0.3-debug2/`. ELF and ROM were unchanged. This confirms
installation, not execution; the next readout must confirm tag `414D4402`, reset
release, CPU progress and the initial ROM vectors after the user launches AMIGA.

The subsequent hardware readout `20261030-051703Z.decoded.json` confirms debug2
(`414D4402`), released resets, PLL/SDRAM/ROM ready, and 92,611,169 CPU memory-state
steps. Its frozen eight entries match the simulated reset vectors and first code
words exactly. However, overlay remains enabled, live CPU addresses have left
the expected ROM range, and the nonblack RGB counter is zero. The user still
observes a valid video signal with a black screen. The reset fault is resolved;
successful Kickstart boot on hardware is not established.

An additional integration test `tests/run_rom_sdram.py` transferred the complete
512 KiB ROM through QSPI, amiga_link and the real SDRAM controller into the chip
model. All 131072 32-bit words matched in six rate/phase cases. This checks the
logical loader/controller boundary, not physical SDRAM timing.

## Debug3 extended capture

Tag `414D4403` retains the existing diagnostic map and adds a frozen 512-entry
block-RAM trace. Each entry contains CPU address, read/write data, and status
sampled on the same memory-state enable. CPU status bit 20 now identifies a
skipped prefetch; such an entry must not be interpreted as a completed bus read.

| Byte address | Contents |
| --- | --- |
| E0 | Selected trace entry: CPU address |
| E4 | Selected entry: read data in upper 16 bits, write data in lower 16 |
| E8 | Selected entry: CPU status, including skip-fetch bit 20 |
| EC | Entry count in bits 9:0, echoed index in 18:10, valid in bit 19 |
| F4 | Read/write trace index, 0–511; affects observation only |
| F8 | Read/write number of memory-state steps to skip before capture |

Default capture starts at step zero and freezes at 512 entries. The indexed RAM
read is synchronous; software checks the echoed index and valid bit. A core
reset clears the count, not the RAM. Changing F8 is intended before a warm reset;
it does not rewind an already frozen capture. `board_trace.py` reads the frozen
entries, writes only F4, checks debug3 identity, and saves raw JSON. The ordinary
first-eight trace remains available unchanged. The matching simulator can save
the 512-entry reference in `tests/boot-build/trace.json`.

## Debug3 hardware trace — October 30, 08:17 UTC

The board reports tag `414D4403`; reset is released and the nonblack RGB count
is still zero. `20261030-081739Z.trace.json` contains all 512 frozen entries.
The first mismatch against the simulation is entry 1: address `00000002`
returns `1114`, expected `4EF9`. The reset-vector reads are `1114 1114 4EF9 00F8`,
so the CPU takes the wrong address (`00F900F8` after its 24-bit address mask).
The low overlay reads use the chipset port (ram_sel=0); subsequent upper-ROM
reads use the fast port (ram_sel=1). All 305 upper-ROM read words checked in
this trace match the ROM file. This points to stale chipset-port data rather
than proving ROM-image corruption. Data-path timing remains under investigation.
The raw registers, trace, comparison and observation are archived in the debug3
release's `reports/hardware-debug3/` directory.

## Chipset-return timing correction in debug4

Instrumentation with the 85 MHz edge evaluated first shows that `SYNC_DELAY=4`
publishes `sdram.dout` at the same nominal edge as CPU1's `ldata_in` capture:
the logical interval is zero. Evaluating both edges together reproduces the
hardware failure: 511 of the 512 meaningful steps match. Only the first stale
word differs (model `0000`, hardware `1114`). Both then consume the preceding
ROM word and jump to `00F900F8`. The older edge-ordered simulation had hidden
this race by making the new register value available without propagation delay.

Debug4 (`414D4404`) uses `SYNC_DELAY=3`. Default single-cycle clk85-to-clk28
constraints now enforce the real 11.696 ns return budget; the clk28-to-clk85
multicycle setup/hold pair is tightened from 4/3 to 3/2. Unrelated upstream
exceptions are retained. The routed build passes with worst reported setup
slack 0.440 ns and hold slack 0.060 ns under these constraints.

`tests/run_clock_margin.py` verifies together, 28-first and 85-first edge ordering,
correct reset vectors, all 512 trace entries and the return interval. The minimum
logical interval is one clk85 period (11.696 ns); the together run also reaches
nonzero RGB by 0.65 simulated seconds. Full ROM loading through the controller,
RGB888 and reset regressions pass. Results and the failing baseline are retained
under `diagnostics/chipset-timing/` and copied into the release. These establish
the logical correction and routed timing result. Hardware confirmation follows.

## Confirmed debug4 hardware boot — October 30, 09:02 UTC

The user confirmed the Kickstart screen with logo and disk drive. Register
readout `20261030-090218Z.decoded.json` identifies `414D4404`, released core/CPU
reset, PLL/SDRAM/ROM ready, overlay disabled, and nonzero RGB. The frozen vector
words are now correctly `1114 4EF9 00F8 00D2`; the first code fetch is `00F800D2`.

All 512 meaningful memory-state steps in `20261030-090232Z.trace.json` match
the debug4 simultaneous-edge reference. All 445 upper-ROM reads checked against
the ROM file match; there is no first divergence. The comparison excludes data
on skipped prefetch steps and compares read or write data according to CPU state.
The raw records, comparison and explicit user screen observation are archived
under `release/0.3-debug4/reports/hardware-debug4/`.

The hardware-confirmed debug4 BIN has SHA-256
`bf8e9214c7b98b2bbc47d30acf133bf7dbee78e413e4fbc5a567625e487b7e86`.
The reset and chipset-return faults are resolved for this bring-up. This result
confirms Kickstart boot with the supplied ROM, not disk emulation, physical
keyboard/mouse/audio validation or exhaustive Amiga compatibility.

## 0.4-debug1 hardware failure and registered SDRAM candidate

The board was left running with a black picture and valid video signal. Captures
`20261030-150643Z.decoded.json` and `20261030-150647Z.trace.json` show released reset,
ROM byte count 524288 and transport CRC FC24AE0D. DF0 is mounted at 901120 bytes,
but its request token and completed sector count remain zero. Only 4 of the 512
meaningful CPU steps match the reference. Address 2 returns 4CF9 instead of 4EF9;
address 4 returns 02F8 instead of 00F8. The first upper-ROM instruction is correct,
but the next words return FFFF. Warm reset reproduces these initial errors in
`20261030-150840Z.decoded.json` / `20261030-150843Z.trace.json`.

This failure occurs before disk access. The transport CRC does not prove that
physical SDRAM stores or returns the same data. Internal routed timing passed,
but the SDC does not model the external SDRAM input/output timing.

The candidate for 0.4-debug2 adds an unconditional 32-bit DQ input register before
the half-word mux, following the input structure used by the working VDAC4 Next
and Slot1 controllers. The original pinned NanoMig tree remains unchanged;
`prepare.py` produces `fpga/sdram.sv`. Reads consume the registered word one state
later; SYNC_DELAY changes from 3 to 2 so chipset data publication keeps the same
11.696 ns logical margin. The request crossing constraint tightens from setup
3/hold 2 to setup 2/hold 1. SDRAM remains 85.5 MHz, CAS 2, PLL phase 270 degrees.

Six full QSPI-to-SDRAM-model ROM writes pass. Three clock-edge orderings produce
correct vectors and identical 512-step CPU traces. These tests establish logical
behavior only. IREG packing, routed timing, full ADF boot and actual board results
must be checked before calling this a hardware fix.

The rising-edge candidate was rejected: placements 2/1 left unrouted nets, and
placement 0 / route 2 completed routing but failed 36 setup endpoints. The worst
was -5.478 ns on the chipset request path to SDRAM (required 23.392 ns versus
28.800 ns data delay). The full eight-second logical ADF boot nevertheless passed;
it is retained under diagnostics/sdram-io/posedge-adf-boot and is not a hardware
success. No candidate BIN has been installed.

The current candidate instead samples DQ on the falling edge before the original
STATE_READ_0. SYNC_DELAY=3 and request setup 3/hold 2 are restored to the confirmed
debug4 schedule. Output publication and burst assembly retain their original
states. The new IREG-to-output path must pass a real half-cycle timing check
(5.848 ns); the chipset return still has 11.696 ns. Frequency and PLL phase remain
85.5 MHz and 270 degrees. Fresh logical regressions and routed STA are in progress.

The falling-edge candidate passed routed timing in build 20261030-160637Z:
0 setup/hold violations, worst setup 0.438 ns, hold 0.060 ns. The worst setup
paths explicitly launch at clk85 falling edge from sdram/dq_q and latch on its
rising edge, with 5.848 ns relationship. PnR reports 52 I/O FFs (32 additional
DQ input registers versus debug1). All three clock-order tests and six full ROM
transfers pass. Four simulated seconds serve 108 sectors and display the Test
Kit 1.21 menu (68020, AGA/PAL, Kickstart 3.1). The prior eight-second rising-edge
simulation is retained separately; it is not the final release's boot evidence.

0.4-debug2 BIN SHA-256:
4b02a86b4164e821b36846443abfaf59b4ed260b4840c15881c7f49c01602887.
The external SDRAM-to-IREG electrical window is not covered by the internal STA result.

## 0.4-debug2 hardware boot, remaining RAM errors, and the Next precedent

The user confirmed the Test Kit 1.21 menu. Hardware capture 20261030-162457Z
shows correct reset vectors and released overlay; 20261030-162505Z.trace.json
matches all 512 simulated CPU steps and all 445 checked upper-ROM reads.
DF0 has completed 108 sectors without an error. This confirms ADF boot, not
memory stability. The user's photograph of F1 Memory shows Random Fill failures
in Fast RAM 0x200000-0x27ffff, D13 in both the 16-bit and 32-bit summaries.
White dots are also visible. adf2-memory-test-state.json records CACR=0: CPU
read hits are disabled, so the Fast RAM read path bypasses the cache data BRAM.
ENABLE_RTC is absent; the reported missing clock is a separate missing feature.

The exact Next fix is commit 1043262010e39d6dc22ed51a956f240ba34c37e3, documented
in E:/zx/Next Tang/next018/release-b5/README.md. It concerned internal bank5_ram
BSRAM bits 3 and 6, not SDRAM: write enable, address mux and CPU data changed
around the BRAM write edge. Capturing b5w_pend/b5w_addr/b5w_data, then writing
on the next edge and holding off lores, eliminated the errors in the recorded
hardware checks (3.4 million CPU reads and 174 million ULA reads).

Amiga already registers SDRAM address, command and DQM. Its write data instead
duplicates one 16-bit register onto two groups of pads; those flops cannot pack
into individual output I/O registers. Candidate 0.4-debug3 adds one unconditional
32-bit output register, loading the selected half from the RAS-latched word and
clearing the masked half. Clearing prevents synthesis merging the two halves.
RAS precedes CAS by two clocks, so the register is ready before WRITE without
moving commands, DQM, acknowledgements or read states. This is a candidate
based on the registered-write principle, not proof of the same electrical fault.

The new focused simulation passes 8192 CPU writes and 8192 burst reads across
all four banks, both physical halfwords and all byte masks, including walking
bits and random data. Six complete QSPI ROM transfers and all three boot clock
orderings also pass. Routed I/O register packing, timing and hardware RAM
stability must be checked separately; the SDRAM model has no electrical timing.

Build 20261030-163841Z used falling-edge IREG and rising-edge OREG. It passed
internal STA (setup 0.980 ns, hold 0.060 ns), but PnR still reported 52 I/O FFs:
the dq_q registers moved to fabric (e.g. D13 at R4C19), so it was rejected before
deployment. Its full ADF simulation was stopped at 1.6 simulated seconds; no
full-boot success is claimed for this candidate. Both IREG and OREG now use
falling-edge clk85. The RAS-latched write word is available before CAS, so this
does not delay the WRITE command or change the byte masks.

## PLL audit: incorrect CLKOUTD3 phase in constraints and simulation

At the user's request the actual PLL configuration was compared with Next and
Slot1 and checked against Gowin UG286-1.9.9E, section 5.1, Figure 5-2 (page 32).
CLKOUTD3 rises on the first falling edge of CLKOUT and divides by three. Our
previous generated clock used only divide_by 3, and the boot simulation aligned
positive edges of clk28 and clk85. Both missed a half clk85 period (5.848 ns).
All earlier logical/STA passes above therefore have this phase limitation.

tests/run_pll.py instantiates the untouched upstream amigaclks.v with Gowin's
GW2A prim_sim.v, including both PLLs and the CLKDIV. After lock it checks 2850
clk28 edges: period85=11.696 ns, period28=35.088 ns, SDRAM phase=8.772 ns,
SDRAM high pulse=5.848 ns, both PLLs locked. The system PLL uses IDIV=6,
FBDIV=19, ODIV=8: PFD=4.5 MHz and VCO=684 MHz, within DS226 limits.
Static DUTYDA_SEL=1000 means 50 percent independently of PSDA_SEL. The
phase-plus-eight rule used in Slot1 applies to dynamic PSDA/DUTYDA ports,
not to these static parameters. The frequency dividers do not need changing.

The SDC now models CLKOUTD3 with edges {2 5 8}; boot simulation starts clk85
high so clk28 rising coincides with clk85 falling, in all three evaluation
orders. Timing and full boot must be rerun under this corrected phase before
packaging. Whether this explains the observed D13 error remains a hardware test.

Build 20261030-165221Z confirms the original-phase problem: clk28 rise is 5.848 ns,
and there are 471 setup violations, worst -4.406 ns (SDRAM dout to Denise sprite
position, 10.184 ns data delay versus 5.848 ns relationship). The request side
also fails: disk length to SDRAM address takes 32.911 ns versus 29.240 ns.
The corresponding four-second logical ADF boot still succeeds (108 sectors),
archived under diagnostics/adf-div3-original-phase, so logic simulation alone
does not establish timing closure. No failing build was deployed.

The next candidate keeps all clock frequencies and changes two static system
PLL settings: PSDA_SEL=1000 (180 degrees), CLKOUTD3_SRC=CLKOUTP. DIV3 then
rises on CLKOUTP's falling edge, aligned with CLKOUT rising. SDC defines the
180-degree CLKOUTP explicitly and derives clk28 from it with edges {2 5 8}.
prepare.py generates fpga/amigaclks.v; upstream remains untouched. The vendor
model checks 2850 aligned edges with 11.696/35.088 ns periods, 5.848 ns SDRAM
phase and 5.848 ns high pulse, both PLLs locked. Clock tests use aligned edges;
boot/ROM simulations clock the chip at inverted clk85 (180 degrees). The CPU
memory test checks both zero and 180-degree chip clocks. These still exclude
analog pad delay and jitter, so actual RAM testing remains necessary.

Aligned-PLL build 20261030-170316Z passes routed STA: 0 setup/hold violations,
worst setup 0.876 ns, hold 0.060 ns. The routed clock summary explicitly gives
clk85 rise 0/fall 5.848 ns, clk28 rise 11.696/fall 29.240 ns. There are 84 I/O
FFs: both 32-bit DQ register banks pack, plus 20 command/address FFs.
BIN SHA-256: adeffce495a04466c437d3d9a5cce09dacd84da69a13aa736fbaf93c40db1b48.
The 8192-write/read CPU test passes for both 0 and 180 degree chip clocks;
all six QSPI ROM transfers pass at 180 degrees. Three boot evaluation orders
produce matching vectors/512-step traces and >=11.696 ns logical chipset margin.
Full ADF boot also passes: four simulated seconds, 108 sectors served, 198 frames
and the Test Kit 1.21 main menu verified visually (68020, AGA/PAL, Kickstart 3.1).
The final run uses the aligned PLL relationship and 180-degree chip clock.
The board subsequently booted the ADF. The user reports no Memory test errors,
but rare visual dots remain during memory and floppy tests. Duration/coverage
was not recorded, and the user could not determine whether the border is affected.
The frozen trace matches 512/512 steps and all 445 upper-ROM reads. Raw captures
and observations are archived in release/0.4-debug3/reports/hardware-adf3.

## Residual visual dots: shared SDRAM traffic and output isolation

tests/run_shared_sdram.py exercises the unchanged debug3 SDRAM controller with
concurrent chipset DMA and CPU traffic, all four banks, both DQ halfwords,
byte masks, burst wrapping, changing data and explicit refresh requests.
Each chip-clock phase (0 and 180 degrees) passes 16384 CPU writes/16384 reads,
32767 chipset writes/21844 reads and 1680 refresh requests. A separate scoreboard
checks data at the chipset deadline and CPU acknowledgement. This checks logic,
not analog SDRAM timing under switching load.

tests/probe_serializer_clocks.py uses the actual PLL/OSER10 vendor model and
sweeps reset phases. It has not established a serializer failure. Its phase
measurements include model time rounding (5 * 7.018 ns differs from 35.088 ns),
which must not be interpreted as physical PLL frequency drift. Unlike Next,
the current serializer uses direct clk28 for PCLK, not the unused CLKDIV output;
changing that connection requires correct pixel-domain transfer and evidence.

Diagnostic candidate 0.4-debug4 adds one LINK word at byte 0x1a0 (word 0x68).
Read-only byte 0x1a4 returns 0x56505231 (VPR1). Writing A5RRGGBB substitutes a
solid RGB888 value directly at the HDMI encoder input, bypassing chipset pixels
and Amber without stopping the CPU, DMA or disk tests. Other high bytes select
ordinary RGB. Power/guest reset clears the word. No memory/PLL/serializer change
is part of this candidate. The control resides in the same clk28 domain as RGB.

board_video_probe.py ADDRESS --source LOCAL --color 202020 --seconds 20 checks
Amiga/VPR1 identity and restores zero in finally; it attempts a fresh connection
if restoration fails. --off explicitly restores the picture. It writes only
0x1a0, verifies identity before every write and records CPU/sync counters plus
readbacks under diagnostics/video-probe. A lost network can prevent restoration;
a guest reset also clears the diagnostic. No board probe has been run yet.

Observe the solid field while the same guest test continues. Dots on it narrow
the fault toward the encoder/serializer/output. A clean field narrows the search
but does not prove SDRAM or Amber is faulty: flat pixels also change HDMI switching
activity. Test multiple colors and restore the original image for comparison.

## Жёлтый экран и окно чтения SDRAM — 31 октября 2026

На 0.4-debug4 Kickstart мигал жёлтым (цвет `$FE5`, ветка `F80404`), запросов секторов DF0 не было.
Выключение питания не помогало. Окна трассы снимались на плате после тёплых сбросов.

Снято на плате: прерывание уровня 3 кладёт на стек в Fast RAM фрейм `SR=0010`, `PC=00F96EE6`,
`format/vector=006C`, то есть `00202282 <- 00F9`, `00202284 <- 6EE6`. RTE читает из `00202282`
значение `00F1`: потерян бит D3 (вывод DQ3, младшая половина). CPU уходит на `00F16EE6`, читает нули
(`ORI.B #0,D0`) со скоростью 2 байта за шаг, доходит до начала ROM и выполняет `JMP F800D2` без сброса.
Позже машина оказывается в жёлтой ветке, которая мигает и через `F80DB8` делает `RESET`. Вход в саму
жёлтую ветку отдельно не снимался. Запись о раннем исключении до старта Exec была неверной: Exec уже работал.
Кэш CPU скрывает такие ошибки памяти, поэтому первые тысячи шагов трассы совпадали с эталоном.

Логика debug4 отличается от debug3 только заливкой RGB и её регистром. SDF обеих сборок (образы
воспроизведены бит в бит с `set_option -gen_sdf 1`) показал:

- такт SDRAM приходит на вывод на 3,8–6,1 нс позже внутреннего фронта (цепь CLKOUTP + OBUF);
- при такте 180° и выборке спадом запас чтения при tAC 6 нс равен setup −0,47 нс, hold +7,43 нс,
  то есть выборка стоит на левом краю окна. Значение tAC взято как предел для такой памяти, на плате
  не измерялось. STA эти пути не проверяет;
- цепь включения выходов `sd_dq` выросла с 1,6–2,4 до 3,9–4,2 нс на младшем байте, но запас до фронта
  такта на выводе остался (около 2,5 нс), причиной она не считается.

Почему debug3 загружалась, а debug4 нет, не установлено: задержки чтения у них одинаковы. Сбой на грани
окна зависит от комбинации данных на шине и от шума. Чтение или запись дали неверный бит, трасса
не различает: виден только бит, полученный CPU.

Исправление 0.4-debug5 (`414D4405`), расписание команд прежнее:

- PLL как в upstream: `CLKOUTD3` от `CLKOUT`, `CLKOUTP` 270° на вывод такта SDRAM;
- весь контроллер SDRAM по спаду `clk85`, вместе с фронтом `clk28`; регистры DQ в I/O по фронту;
  такт SDRAM теперь на 90° после рабочего фронта контроллера;
- выводы DQ включаются на такт раньше WRITE.

Запасы по SDF: чтение setup +2,45 нс, hold +3,27 нс; команды setup от 1,29 нс; включение DQ до WRITE
от 9,37 нс. Routed STA: 0 нарушений, setup 1,14 нс, hold 0,06 нс, 84 регистра I/O.
Модель проходит те же 9 528 503 шага до меню Test Kit, что и debug4; задержек выводов она не учитывает.

На плате (BIN SHA-256 `b4f99ccdccb0d95fcaad7fd6d28c5ffa8e2b7a4aac4d9701d912e38749459e69`):
Test Kit загрузился, обслужено 108 секторов DF0, регистры подтвердили `414D4405`. По словам
пользователя, F1 Memory прошёл 3 круга без ошибок, картинка под этой нагрузкой чистая.

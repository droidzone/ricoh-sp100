# Ricoh SP 100 / SP 111 / SP 111SU / SP 112 / SP 204 driver for CUPS

A native C driver for Ricoh's "DDST" GDI laser printers. These printers do not
understand PostScript or PCL: they expect a bitmap of each page, compressed
with JBIG and wrapped in a few PJL commands.

The MFP models (SP 111SU, SP 112SU) also have a scanner. Scanning is already
supported by SANE's built-in `ricoh2` backend (`scanimage -L`, Simple Scan,
XSane), so this project only covers printing. The scanner and the printer are
separate USB interfaces (0 and 1), so they never conflict.

## How it works

This follows the standard architecture used by most Linux printer drivers
(e.g. CUPS' own `rastertolabel`/`rastertoepson`, brlaser, splix, foo2zjs):

```
application ─PDF─▶ cupsd ─▶ pdftopdf ─▶ gstoraster ─CUPS raster─▶ rastertoricohddst ─PJL+JBIG─▶ usb backend ─▶ printer
                              (CUPS / cups-filters, ships with the OS)        (this driver, C)
```

* **`ppd/ricoh-ddst.drv`** describes the printers (paper sizes, margins,
  resolution, trays) in CUPS' driver-information format. `ppdc` compiles it
  into one PPD per model. The PPD asks CUPS for 1-bit black raster at 600 dpi
  and names the filter.
* **`src/rastertoricohddst.c`** is the filter. It reads CUPS raster pages from
  stdin and writes the printer's language to stdout, compressing each page in
  memory with libjbig. No temporary files, no external tools, no shell or
  scripting languages. Each page goes to the printer as soon as it's
  compressed.
* CUPS handles everything else: rendering (Ghostscript, one pass straight from
  PDF), copies and collation, n-up, page ranges, and talking to the USB port.

### Compared to the old `pstoricohddst-gdi` shell filter (in `legacy/`)

Timings for a 10-page document on the same machine:

|                              | old shell filter          | new C filter   |
|------------------------------|---------------------------|----------------|
| Rendering                    | PDF→PS→PS→PBM (Ghostscript 2–3×) | PDF→raster (once) |
| Per-page work                | 3–4 ImageMagick runs + `pbmtojbg` via temp files | in-memory |
| Time to process the job      | 8.3 s                     | 0.9 s          |
| Hard-coded `sleep 30` per job | yes (blocks the queue)   | no             |
| Page order                   | `find` order (could be shuffled) | always correct |
| Cancel mid-job               | could leave the printer mid-page | finishes the page and ends the job cleanly |
| Toner dot count              | ImageMagick `fx:mean` (slow) | exact pixel count |

The bytes sent to the printer use exactly the same PJL commands and JBIG
settings (`pbmtojbg -p 72 -o 3 -m 0 -q`) as before, and the same full-page
bitmap size. `make check` verifies this.

## Requirements

Arch: `pacman -S cups cups-filters ghostscript jbigkit`
Debian/Ubuntu: `apt install cups printer-driver-cups-filters ghostscript libcups2-dev libjbig-dev`

Building needs a C compiler, `make`, `pkg-config`, and the CUPS/jbigkit headers.

## Build, test, install

```sh
make              # builds the filter and the PPDs
make check        # offline test, nothing is printed
sudo make install # filter -> /usr/lib/cups/filter, PPDs -> /usr/share/ppd/ricoh-ddst
```

Then point an existing queue at the new PPD (the USB address is kept):

```sh
sudo lpadmin -p RICOH_SP_111SU_DDST -P /usr/share/ppd/ricoh-ddst/rsp111su.ppd -E
```

Or add the printer in the CUPS web UI (http://localhost:631) or your desktop's
printer settings, and pick *Ricoh SP 111SU, rastertoricohddst 2.0*.

| Model      | PPD            |
|------------|----------------|
| SP 100     | `rsp100.ppd`   |
| SP 111     | `rsp111.ppd`   |
| SP 111SU   | `rsp111su.ppd` |
| SP 112     | `rsp112.ppd`   |
| SP 204     | `rsp204.ppd` (adds bypass tray and 1200×600 dpi) |

To uninstall: `sudo make uninstall`.

## Print options

* **Media Size**: A4, A5, A6, Letter, Legal, JIS B5/B6, Envelope Monarch.
* **Halftone**:
  * *Standard* (default): Ghostscript renders black and white directly.
    Best for text and line art.
  * *Photo*: Ghostscript renders 8-bit gray, and the filter converts it with
    a clustered-dot screen (85 lpi, 45°) plus a tone curve that offsets
    toner spread. Grays and photos come out smoother, and dark shades stay
    distinguishable. Text is identical in both modes.

  Pick it in the print dialog, or on the command line:
  `lp -o Halftone=Photo file.pdf`. To make it the default for the queue:
  `lpoptions -p RICOH_SP_111SU_DDST -o Halftone=Photo`.

## Troubleshooting

* Filter messages are in CUPS' log: `journalctl -u cups` or
  `/var/log/cups/error_log`.
* To inspect what would be sent to the printer without printing:

  ```sh
  PPD=ppd/rsp111su.ppd /usr/lib/cups/filter/gstoraster 1 me title 1 "" file.pdf \
    | ./rastertoricohddst 1 me title 1 "" > out.prn
  make ddst-check && ./ddst-check out.prn
  ```

## Credits

Protocol reverse-engineered by Serge V Shistarev (madlynx) in the original
`pstoricohddst-gdi` filter, with contributions from Joel G Mathew, James
Bottomley, mishta-real and others. The original files are kept in `legacy/`.

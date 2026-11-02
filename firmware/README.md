# firmware/

Put YOUR OWN flash dump here as `dump.bin` (the instructions in `doom/README.md` do this), and the update files the patcher writes go here too. Everything in this folder
is git-ignored (`firmware/*.bin`), so none of it is committed by accident. No firmware is distributed with this repository.

Some tools use `dump.bin` as the reference image: `usb_flash_read.py` (compare a USB read), `find_cc_target.py`, `make_symbols.py`, `test_update_model.py`, `make_public_dump.py`.
Read it with `tools/usb_flash_read.py` (USB) or a programmer (4 MiB SOIC-8 SPI NOR, JEDEC ID `C4 60 16`).

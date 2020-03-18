# TODO (Khangaroo): Make this process a lot less hacky (no, export did not work)
# See MakefileNSO

.PHONY: all clean skyline skyline_patch send

CROSSVER ?= 120
IP ?= 192.168.1.9

all: skyline skyline_patch send

skyline:
	$(MAKE) all -f MakefileNSO CROSSVER=$(CROSSVER)

skyline_patch: patches/*.slpatch patches/configs/$(CROSSVER).config patches/maps/$(CROSSVER)/*.map \
								build$(CROSSVER)/$(shell basename $(CURDIR))$(CROSSVER).map scripts/genPatch.py
	@rm -f aldebaran_patch_$(CROSSVER)/*.ips
	python3 scripts/genPatch.py $(CROSSVER)

send: all
	python3 scripts/sendPatch.py $(IP) $(CROSSVER)

clean:
	$(MAKE) clean -f MakefileNSO
	@rm -fr aldebaran_patch_*

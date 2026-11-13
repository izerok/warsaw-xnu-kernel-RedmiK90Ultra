# panelxml — dtsi → Silicium panel XML converter

`dtsi2panelxml.py` converts a Qualcomm kernel panel `.dtsi` (as published
in MiCode's `vendor_qcom_opensource_display-devicetree`) into the
QCOM-UEFI DisplayDxe Panel XML that Mu-Silicium packages carry in
`<device>Pkg/RawFiles/`.

It is adapted from annibale's own generator (`tools/dts2panelxml.py` in
iola1999/hackios-redmi-k90-annibale), which produced annibalePkg's
shipped `Panel_xiaomi_p11_42_0d_0a_dsc_cmd.xml` from a merged DTS dumped
off the phone. The output template is that generator's, unchanged; only
the input side differs:

- input is the source `.dtsi` (decimal or hex cells, comments resolved,
  the per-timing `#include` of the `*-common.dtsi` inlined), not a
  decompiled DTB dump;
- the timing mode is chosen by `--framerate` (default **60** — the mode
  the shipped P11 XML was made from: its `DSIRefreshRate` is `0x3c0000`
  and its init sequence is the 60 Hz mode's `qcom,mdss-dsi-on-command`);
- `PanelName` defaults to `qcom,mdss-dsi-panel-name` minus the trailing
  ` cmd mode dsc dsi panel` (override with `--panel-name`);
- lane count is counted from the `qcom,mdss-dsi-lane-N-state` properties.

Template constants taken from the original (InterfaceType 9,
DSIChannelId 2, DSIColorFormat 36, DSITrafficMode 1, …) are guarded by
assertions on the corresponding dtsi properties (cmd-mode panel type,
bpp 30, burst traffic mode, lane_map_0123, DSC compression, TE/LP11
flags present). Inputs outside the evidenced envelope are refused, not
silently converted.

## Usage

```sh
# warsaw p12x (already generated; result lives in
# uefi/warsawPkg/RawFiles/Panel_xiaomi_p12x_42_02_0a_dsc_cmd.xml)
python3 tools/panelxml/dtsi2panelxml.py \
  display/warsaw/dsi-panel-p12x-42-02-0a-dsc-cmd.dtsi out.xml

# regression: reproduce annibale's shipped P11 XML byte-for-byte
python3 tools/panelxml/dtsi2panelxml.py \
  display/dsi-panel-p11-42-0d-0a-p10-dsc-cmd.dtsi /tmp/p11.xml \
  --verify-against <mu-tree>/Platforms/Xiaomi/annibalePkg/RawFiles/Panel_xiaomi_p11_42_0d_0a_dsc_cmd.xml
```

Note the regression input is the **p10 build variant** of the P11
panel: the base and p00 variants carry a shorter on-command and do not
reproduce the shipped XML. See `PHASE8-PANELXML.md` in the goal's
hidden_files for the full field-mapping table and validation record.

## Format notes

- The output is an XML *fragment* (sibling top-level elements, no single
  root) — that is the DisplayDxe panel format, identical in shape to
  the shipped P11 file. To parse it with a strict XML parser, wrap it
  in a synthetic root element first.
- Kernel command arrays serialise each command as
  `[type, last, vc, ack, wait, 0, len, data...]`; the XML line is
  `type data...` plus the wait as a trailing **decimal** when nonzero.
  The converter refuses a nonzero byte 5 (outside the evidenced
  layout) instead of guessing.
- The panel off-command is *not* serialised: the shipped format has no
  term/off sequence (the in-package P11 XML ends at
  `</DSIInitSequence>`), and neither does the output.

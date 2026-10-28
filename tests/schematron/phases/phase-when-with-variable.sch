<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="items" value="count(//item)"/>
  <sch:phase id="one" when="$items = 1 and doc"><sch:active pattern="a"/></sch:phase>
  <sch:phase id="three" when="$items = 3 and doc"><sch:active pattern="c"/></sch:phase>
  <sch:pattern id="a">
    <sch:rule context="item">
      <sch:report test="@n = 1">Pattern a.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="b">
    <sch:rule context="item">
      <sch:report test="@n = 2">Pattern b.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="c">
    <sch:rule context="item">
      <sch:report test="@n = 3">Pattern c.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>

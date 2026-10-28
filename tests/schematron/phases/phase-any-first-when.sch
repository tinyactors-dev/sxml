<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="small" when="count(//item) &lt; 2"><sch:active pattern="a"/></sch:phase>
  <sch:phase id="medium" when="count(//item) = 3"><sch:active pattern="b"/></sch:phase>
  <sch:phase id="also-medium" when="count(//item) &gt; 2"><sch:active pattern="c"/></sch:phase>
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

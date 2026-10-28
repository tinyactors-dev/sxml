<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3" defaultPhase="later">
  <sch:phase id="first"><sch:active pattern="a"/></sch:phase>
  <sch:phase id="later"><sch:active pattern="b"/><sch:active pattern="c"/></sch:phase>
  <sch:phase id="nothing"/>
  <sch:phase id="again"><sch:active pattern="a"/><sch:active pattern="c"/></sch:phase>
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

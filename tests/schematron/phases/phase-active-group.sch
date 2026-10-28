<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="g"><sch:active pattern="grp"/></sch:phase>
  <sch:group id="grp">
    <sch:rule context="item[@n = 2]">
      <sch:report test="true()">Group rule one.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:report test="@n = 2">Group rule two.</sch:report>
    </sch:rule>
  </sch:group>
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

<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="second">
    <sch:active pattern="two"/>
  </sch:phase>
  <sch:pattern abstract="true" id="t">
    <sch:rule context="item">
      <sch:report test="@n = $k">Found $k.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="one" is-a="t">
    <sch:param name="k" value="1"/>
  </sch:pattern>
  <sch:pattern id="two" is-a="t">
    <sch:param name="k" value="2"/>
  </sch:pattern>
</sch:schema>

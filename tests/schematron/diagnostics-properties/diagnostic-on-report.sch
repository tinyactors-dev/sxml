<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 1" diagnostics="one">One.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="one" role="note">A note about one.</sch:diagnostic>
  </sch:diagnostics>
</sch:schema>

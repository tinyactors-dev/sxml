<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:report flag="alpha beta" test="@n = 1">One.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>

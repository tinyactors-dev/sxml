<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xpath3">
  <sch:pattern>
    <sch:rule context="/doc/item">
      <sch:report test="@n = 3">Three.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>

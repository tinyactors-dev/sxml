<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:rule context="doc">
      <sch:assert test="count(item) = 2">Two items.</sch:assert>
    </sch:rule>
  </sch:group>
</sch:schema>

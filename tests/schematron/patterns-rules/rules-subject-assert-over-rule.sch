<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="order" subject="total">
      <sch:assert test="false()" subject="line[2]">On the second line.</sch:assert>
      <sch:assert test="false()">On the total.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>

<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="order">
      <sch:assert test="total &gt; 0" subject="total">The <sch:name/> is positive, not <sch:value-of select="."/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>

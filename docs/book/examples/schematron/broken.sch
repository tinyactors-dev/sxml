<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="line[@quantity">
      <sch:assert test="@price &gt;">A price.</sch:assert>
    </sch:rule>
    <sch:rule context="invoice">
      <sch:asert test="@total">A total.</sch:asert>
    </sch:rule>
  </sch:pattern>
</sch:schema>

<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="section">
      <sch:assert test="title">A section has a title.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>

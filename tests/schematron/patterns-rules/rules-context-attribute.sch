<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="@n">
      <sch:assert test=". &lt; 3">An n below 3.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>

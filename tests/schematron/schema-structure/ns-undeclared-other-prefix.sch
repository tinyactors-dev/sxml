<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" xmlns:output="http://www.w3.org/2010/xslt-xquery-serialization" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="empty(output:item)">No serialization element.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>

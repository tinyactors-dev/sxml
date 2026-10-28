<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern id="remote" documents="'svrl-subordinate-document/sub.xml'">
    <sch:rule context="item">
      <sch:report test="@n = 5">Five.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>

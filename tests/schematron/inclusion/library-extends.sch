<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:extends href="library-extends/library.sch"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:extends rule="has-n"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>

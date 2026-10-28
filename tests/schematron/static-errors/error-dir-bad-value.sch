<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n">An item <sch:dir value="up">has</sch:dir> an n attribute.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>

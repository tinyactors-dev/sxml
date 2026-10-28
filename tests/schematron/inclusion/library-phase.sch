<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:extends href="library-phase/library.sch"/>
  <sch:phase id="only-library"><sch:active pattern="library-pattern"/></sch:phase>
  <sch:pattern id="local">
    <sch:rule context="item">
      <sch:report test="true()">Local.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>

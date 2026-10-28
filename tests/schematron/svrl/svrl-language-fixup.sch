<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3" xml:lang="en">
  <sch:pattern>
    <sch:rule context="doc">
      <sch:assert test="item">Has items.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:include href="svrl-language-fixup/german.sch#deutsch"/>
</sch:schema>

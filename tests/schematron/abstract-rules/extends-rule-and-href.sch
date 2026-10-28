<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="has-n">
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
    <sch:rule context="item">
      <sch:extends rule="has-n" href="extends-rule-and-href.sch#has-n"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>

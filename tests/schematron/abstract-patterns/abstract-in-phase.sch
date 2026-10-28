<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="tables-phase"><sch:active pattern="tables"/></sch:phase>
  <sch:pattern abstract="true" id="container">
    <sch:rule context="$parent">
      <sch:assert test="$child">The <sch:name/> has children.</sch:assert>
    </sch:rule>
    <sch:rule context="$child">
      <sch:assert test="$grandchild">The <sch:name/> has a grandchild.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="container" id="tables">
    <sch:param name="parent" value="table"/>
    <sch:param name="child" value="row"/>
    <sch:param name="grandchild" value="cell"/>
  </sch:pattern>
  <sch:pattern is-a="container" id="lists">
    <sch:param name="parent" value="list"/>
    <sch:param name="child" value="entry"/>
    <sch:param name="grandchild" value="missing"/>
  </sch:pattern>
</sch:schema>

<?xml version="1.0" encoding="UTF-8"?>
<!-- Written as older rule sets often are: one pattern holds the
     variables that the others read, and a let follows an assertion. -->
<schema xmlns="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt2">
  <ns prefix="sc" uri="http://www.w3.org/2005/07/scxml"/>

  <pattern id="variables">
    <let name="state-ids" value="//sc:state/@id | //sc:final/@id"/>
  </pattern>

  <pattern id="initial">
    <rule context="sc:scxml">
      <assert id="initial-exists" test="@initial = $state-ids">The
        initial state <value-of select="@initial"/> is no state.</assert>
      <let name="names" value="//sc:state/@id ! string()"/>
      <assert id="distinct" test="count($names) = count(distinct-values($names))">Two
        states have one name.</assert>
    </rule>
  </pattern>
</schema>

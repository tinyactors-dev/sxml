<?xml version="1.0" encoding="UTF-8"?>
<!-- A schema with a mistake on every rule. -->
<schema xmlns="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <ns prefix="sc" uri="http://www.w3.org/2005/07/scxml"/>
  <pattern id="mistakes">
    <rule context="sc:state[">
      <assert test="@id">A state has an id.</assert>
    </rule>
    <rule context="sc:transition">
      <assert test="vane:known(@event)">The event is known.</assert>
    </rule>
    <rule context="sc:data">
      <assert test="@expr" diagnostics="hint">A data item has a value.</assert>
      <check test="@id"/>
    </rule>
  </pattern>
</schema>

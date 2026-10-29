<?xml version="1.0" encoding="UTF-8"?>
<!-- The names a Vane machine uses, checked with the shared rules of
     vane-rules.sch. -->
<schema xmlns="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3"
        schematronEdition="2025">
  <title>Names of a Vane machine</title>
  <ns prefix="sc" uri="http://www.w3.org/2005/07/scxml"/>
  <ns prefix="vane" uri="urn:example:vane"/>
  <ns prefix="h" uri="http://www.w3.org/1999/xhtml"/>

  <!-- The abstract pattern and the abstract rule. -->
  <extends href="vane-rules.sch"/>

  <!-- The draft the machine implements. -->
  <param name="draft" value="'vane.xml'"/>

  <!-- Only the states and the transitions between them. -->
  <phase id="graph">
    <active pattern="states"/>
    <active pattern="targets"/>
  </phase>

  <include href="reachable.sch"/>

  <pattern is-a="names" id="events">
    <param name="context" value="sc:transition[@event]"/>
    <param name="names" value="@event"/>
    <param name="known" value="//vane:event/@name"/>
    <param name="what" value="'declared event'"/>
  </pattern>

  <pattern is-a="names" id="targets">
    <param name="context" value="sc:transition[@target]"/>
    <param name="names" value="@target"/>
    <param name="known" value="//(sc:state | sc:final)/@id"/>
    <param name="what" value="'state'"/>
  </pattern>

  <pattern is-a="names" id="requirements">
    <param name="context" value="*[@vane:implements]"/>
    <param name="names" value="@vane:implements"/>
    <param name="known" value="doc($draft)//t/@anchor"/>
    <param name="what" value="'requirement of the draft'"/>
  </pattern>

  <pattern id="documented">
    <rule context="sc:state | vane:event">
      <extends rule="has-documentation"/>
    </rule>
  </pattern>
</schema>

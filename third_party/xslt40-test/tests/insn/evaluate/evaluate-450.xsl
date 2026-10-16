<?xml version="1.0"?> 
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="4.0"
  fixed-namespaces="#standard">

  <!-- PURPOSE test xsl:evaluate trusted attribute -->

  <xsl:param name="xpath" static="yes" as="xs:string" required="yes"/>
  <xsl:param name="trusted" static="yes" as="xs:boolean" required="yes"/>

  <xsl:template name="main">
    <out>
      <xsl:evaluate xpath="$xpath" trusted="{$trusted}"/>
    </out>
  </xsl:template>



</xsl:stylesheet>
